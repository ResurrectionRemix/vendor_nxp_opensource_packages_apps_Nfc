/*
 * Copyright (C) 2015-2018 NXP Semiconductors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <android-base/stringprintf.h>
#include <base/logging.h>
#include "TransactionController.h"
#include <time.h>
#include <string.h>

using android::base::StringPrintf;

extern bool nfc_debug_enabled;
namespace android {
extern void* enableThread(void* arg);
extern Transcation_Check_t* nfcManager_transactionDetail(void);
extern bool nfcManager_isRequestPending(void);
}  // namespace android

/*Transaction Controller Instance Reference*/
transactionController* transactionController::pInstance = NULL;

/*******************************************************************************
 **
 ** Function:       lastRequestResume
 **
 ** Description:   This function forks a thread which resumes last request after
 **                transaction is terminated
 **
 ** Returns:     None
 **
 *******************************************************************************/
void transactionController::lastRequestResume(void) {
  pthread_t transaction_thread;
  int irret = -1;
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s", __FUNCTION__);

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pendingTransHandleTimer->kill();
  pendingTransHandleTimer = new IntervalTimer();

  // Fork a thread which shall abort a stuck transaction and resume last
  // trasaction*/
  irret =
      pthread_create(&transaction_thread, &attr, android::enableThread, NULL);
  if (irret != 0) {
    LOG(ERROR) << StringPrintf("Unable to create the thread");
  }
  pthread_attr_destroy(&attr);
  pTransactionDetail->current_transcation_state = NFA_TRANS_DM_RF_TRANS_END;
}
/*******************************************************************************
 **
 ** Function:       transactionHandlePendingCb
 **
 ** Description:    This is a callback function registered against
 **                 pendingTransHandleTimer timer. This timer handler
 **                 triggers enable_thread for handling pending transactions
 **
 ** Returns:       None
 **
 *******************************************************************************/

void transactionController::transactionHandlePendingCb(union sigval) {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("Inside %s", __FUNCTION__);

  pInstance->lastRequestResume();
}
/*******************************************************************************
 **
 ** Function:       transactionAbortTimerCb
 **
 ** Description:    This is a callback function registered against abort timer
 **
 ** Returns:       None
 **
 *******************************************************************************/

void transactionController::transactionAbortTimerCb(union sigval) {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("Inside %s", __FUNCTION__);

  pInstance->transactionTerminate(TRANSACTION_REQUESTOR(exec_pending_req));
}

/*******************************************************************************
 **
 ** Function:       transactionController
 **
 ** Description:   Constructor which creates the transaction controller
 **
 ** Returns:      None
 **
 *******************************************************************************/

transactionController::transactionController(void) {
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s: transaction controller created", __FUNCTION__);

  sem_init(&barrier, 0, 1);

  pTransactionDetail = android::nfcManager_transactionDetail();
  abortTimer = new IntervalTimer();
  pendingTransHandleTimer = new IntervalTimer();
  requestor = NO_REQUESTOR;
}
/*******************************************************************************
 **
 ** Function:       transactionLiveLockable
 **
 ** Description:   Checks whether the transaction is long lasting
 **
 ** Returns:        true/false
 **
 *******************************************************************************/
bool transactionController::transactionLiveLockable(
    eTransactionId transactionRequestor) {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s: Performing check for long duration transaction", __FUNCTION__);
  return ((transactionRequestor == NFA_ACTIVATED_EVENT) ||
          (transactionRequestor == NFA_EE_ACTION_EVENT) ||
          (transactionRequestor == NFA_TRANS_CE_ACTIVATED_EVENT) ||
          (transactionRequestor == RF_FIELD_EVT));
}
/*******************************************************************************
 **
 ** Function:       transactionStartBlockWait
 **
 ** Description:    The caller of the function block waits to start a
 *transaction
 **
 ** Returns:        None
 **
 *******************************************************************************/
bool transactionController::transactionAttempt(
    eTransactionId transactionRequestor, unsigned int timeoutInSec) {
  struct timespec timeout = {.tv_sec = 0, .tv_nsec = 0};
  int semVal = 0;

  // Get current time
  clock_gettime(CLOCK_REALTIME, &timeout);

  // Set timeout factor relative to current time
  timeout.tv_sec += timeoutInSec;

  sem_getvalue(&barrier, &semVal);
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s: Transaction attempted : %d when barrier is: %d",
                      __FUNCTION__, transactionRequestor, semVal);

  if (pendingTransHandleTimer->isRunning()) {
    DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s: Transaction denied due to pending transaction: %d ", __FUNCTION__,
        transactionRequestor);
    return false;
  }
  // Block wait on barrier
  if (sem_timedwait(&barrier, &timeout) != 0) {
    DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s: Transaction denied : %d ", __FUNCTION__, transactionRequestor);
    return false;
  }

  pTransactionDetail->trans_in_progress = true;
  requestor = transactionRequestor;

  // In case there is a chance that transaction will be stuck; start transaction
  // abort timer
  if (transactionLiveLockable(transactionRequestor)) {
    abortTimer->set(1000000, transactionAbortTimerCb);
  }
  sem_getvalue(&barrier, &semVal);
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s: Transaction granted : %d and barrier is: %d",
                      __FUNCTION__, transactionRequestor, semVal);
  return true;
}
/*******************************************************************************
 **
 ** Function:       transactionAttempt
 **
 ** Description:   Caller of this function will try to start a transaction(if
 *none is ongoing)
 **
 ** Returns:      true: If transaction start attempt is successful
 **                  false: If transaction attempt fails
 **
 *******************************************************************************/
bool transactionController::transactionAttempt(
    eTransactionId transactionRequestor) {
  int semVal = 0;

  sem_getvalue(&barrier, &semVal);
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s: Transaction attempted : %d when barrier is: %d",
                      __FUNCTION__, transactionRequestor, semVal);

  if (pendingTransHandleTimer->isRunning()) {
    DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s: Transaction denied due to pending transaction: %d ", __FUNCTION__,
        transactionRequestor);
    return false;
  }

  if (sem_trywait(&barrier) == 0) {
    pTransactionDetail->trans_in_progress = true;

    if (transactionLiveLockable(transactionRequestor)) {
      abortTimer->set(1000000, transactionAbortTimerCb);
    }
    DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s: Transaction granted : %d ", __FUNCTION__, transactionRequestor);

    requestor = transactionRequestor;
    sem_getvalue(&barrier, &semVal);
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s: Transaction granted : %d and barrier is: %d",
                        __FUNCTION__, transactionRequestor, semVal);
    return true;
  }
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s: Transaction denied : %d ", __FUNCTION__, transactionRequestor);
  return false;
}
/*******************************************************************************
 **
 ** Function:       transactionEnd
 **
 ** Description:    If the transaction requestor has started the transaction
 *then this function ends it.
 **
 ** Returns:        None
 **
 *******************************************************************************/
void transactionController::transactionEnd(
    eTransactionId transactionRequestor) {
  int val;
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s: Enter", __FUNCTION__);
  if (requestor == transactionRequestor) {
    /*If any abort timer is running for this transaction then stop it*/
    abortTimer->kill();
    /*Create new abort timer for next use*/
    abortTimer = new IntervalTimer();
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s: Transaction control timer killed", __FUNCTION__);

    pTransactionDetail->trans_in_progress = false;
    requestor = NO_REQUESTOR;
    DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s: Transaction ended : %d ", __FUNCTION__, transactionRequestor);

    /*
    ** TODO: The below code needs to improved and thread dependency shall be
    *reduced
    **/
    if (android::nfcManager_isRequestPending()) {
      pendingTransHandleTimer->set(1, transactionHandlePendingCb);
    }

    sem_getvalue(&barrier, &val);
    if (!val) sem_post(&barrier);
  }
}
/*******************************************************************************
 **
 ** Function:       transactionTerminate
 **
 ** Description:   This function shall recover transaction from a stuck state
 **
 ** Returns:        true: If attempt to terminate is successful
 **                 false: If attempt to terminate fails
 **
 *******************************************************************************/
bool transactionController::transactionTerminate(
    eTransactionId transactionRequestor) {
  int val;

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s: Enter. Requested by : %d ", __FUNCTION__, transactionRequestor);

  if ((requestor != 0) && (requestor == transactionRequestor ||
                           transactionRequestor == exec_pending_req)) {
    pTransactionDetail->trans_in_progress = false;
    requestor = NO_REQUESTOR;
    killAbortTimer();

    if (android::nfcManager_isRequestPending()) {
      pendingTransHandleTimer->set(1, transactionHandlePendingCb);
    }

    sem_getvalue(&barrier, &val);
    if (!val) sem_post(&barrier);
    DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s: Transaction terminated : %d ", __FUNCTION__, transactionRequestor);
    return true;
  }
  return false;
}
/*******************************************************************************
 **
 ** Function:       transactionInProgress
 **
 ** Description:    Checks whether transaction is in progress
 **
 ** Returns:         true/false
 **
 *******************************************************************************/
bool transactionController::transactionInProgress(void) {
  return (pTransactionDetail->trans_in_progress == true);
}
/*******************************************************************************
 **
 ** Function:       getInstance
 **
 ** Description:    Checks whether transaction is in progress
 **
 ** Returns:         transaction controller instance
 **
 *******************************************************************************/
transactionController* transactionController::getInstance(void) {
  return pInstance;
}
/*******************************************************************************
 **
 ** Function:       controller
 **
 ** Description:   This routine initializes transaction controller fields on
 *every
 **                invocation.
 **
 ** Returns:       Reference to transaction controller
 **
 *******************************************************************************/
transactionController* transactionController::controller(void) {
  if (pInstance == NULL) {
    pInstance = new transactionController();
  } else {
    pInstance->pTransactionDetail->trans_in_progress = false;
    pInstance->requestor = NO_REQUESTOR;
    pInstance->abortTimer->kill();
    pInstance->pendingTransHandleTimer->kill();
    pInstance->abortTimer = new IntervalTimer();
    pInstance->pendingTransHandleTimer = new IntervalTimer();

    sem_destroy(&pInstance->barrier);

    sem_init(&pInstance->barrier, 0, 1);

    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s: transaction controller initialized", __FUNCTION__);
  }
  memset(pInstance->pTransactionDetail, 0x00,
         sizeof(*(pInstance->pTransactionDetail)));
  return pInstance;
}
/*******************************************************************************
 **
 ** Function:       killAbortTimer
 **
 ** Description:    This function kills transaction abort timer
 **
 ** Returns:        None
 **
 *******************************************************************************/
void transactionController::killAbortTimer(void) {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s: transaction controller abort timer killed", __FUNCTION__);

  if (transactionInProgress()) {
    abortTimer->kill();
    abortTimer = new IntervalTimer();
  }
}
/*******************************************************************************
 **
 ** Function:       setAbortTimer
 **
 ** Description:   This function starts transaction timer for specified timeout
 *value
 **
 ** Returns:       None
 **
 *******************************************************************************/
void transactionController::setAbortTimer(unsigned int msec) {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s: transaction controller abort timer set", __FUNCTION__);

  if (transactionInProgress()) abortTimer->set(msec, transactionAbortTimerCb);
}

/*******************************************************************************
 **
 ** Function:       getCurTransactionRequestor
 **
 ** Description:   This function returns current active requestor
 **
 ** Returns:       active requestor
 **
 *******************************************************************************/
eTransactionId transactionController::getCurTransactionRequestor() {
  return requestor;
}
