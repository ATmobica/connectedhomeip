/*
 *
 *    Copyright (c) 2023 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include <app/InteractionModelEngine.h>
#include <app/reporting/ReportSchedulerImpl.h>
#include <app/reporting/SynchronizedReportSchedulerImpl.h>
#include <app/tests/AppTestContext.h>
#include <lib/support/UnitTestContext.h>
#include <lib/support/UnitTestRegistration.h>
#include <lib/support/logging/CHIPLogging.h>
#include <nlunit-test.h>

namespace {

class TestContext : public chip::Test::AppContext
{
public:
    static int Initialize(void * context)
    {
        if (AppContext::Initialize(context) != SUCCESS)
            return FAILURE;

        auto * ctx = static_cast<TestContext *>(context);

        if (ctx->mEventCounter.Init(0) != CHIP_NO_ERROR)
        {
            return FAILURE;
        }

        return SUCCESS;
    }

    static int Finalize(void * context)
    {
        chip::app::EventManagement::DestroyEventManagement();

        if (AppContext::Finalize(context) != SUCCESS)
            return FAILURE;

        return SUCCESS;
    }

private:
    chip::MonotonicallyIncreasingCounter<chip::EventNumber> mEventCounter;
};

class NullReadHandlerCallback : public chip::app::ReadHandler::ManagementCallback
{
public:
    void OnDone(chip::app::ReadHandler & apReadHandlerObj) override {}
    chip::app::ReadHandler::ApplicationCallback * GetAppCallback() override { return nullptr; }
};

} // namespace

namespace chip {
namespace app {
namespace reporting {

using ReportScheduler     = reporting::ReportScheduler;
using ReportSchedulerImpl = reporting::ReportSchedulerImpl;
using ReadHandlerNode     = reporting::ReportScheduler::ReadHandlerNode;
using Milliseconds64      = System::Clock::Milliseconds64;

static const size_t kNumMaxReadHandlers = 16;

class TestTimerDelegate : public ReportScheduler::TimerDelegate
{
public:
    struct NodeTimeoutPair
    {
        ReadHandlerNode * node;
        System::Clock::Timeout timeout;
    };

    NodeTimeoutPair mPairArray[kNumMaxReadHandlers];
    size_t mPairArraySize                         = 0;
    System::Clock::Timestamp mMockSystemTimestamp = System::Clock::Milliseconds64(0);

    NodeTimeoutPair * FindPair(ReadHandlerNode * node, size_t & position)
    {
        for (size_t i = 0; i < mPairArraySize; i++)
        {
            if (mPairArray[i].node == node)
            {
                position = i;
                return &mPairArray[i];
            }
        }
        return nullptr;
    }

    CHIP_ERROR insertPair(ReadHandlerNode * node, System::Clock::Timeout timeout)
    {
        VerifyOrReturnError(mPairArraySize < kNumMaxReadHandlers, CHIP_ERROR_NO_MEMORY);
        mPairArray[mPairArraySize].node    = node;
        mPairArray[mPairArraySize].timeout = timeout;
        mPairArraySize++;

        return CHIP_NO_ERROR;
    }

    void removePair(ReadHandlerNode * node)
    {
        size_t position;
        NodeTimeoutPair * pair = FindPair(node, position);
        VerifyOrReturn(pair != nullptr);

        size_t nextPos = static_cast<size_t>(position + 1);
        size_t moveNum = static_cast<size_t>(mPairArraySize - nextPos);

        // Compress array after removal, if the removed position is not the last
        if (moveNum)
        {
            memmove(&mPairArray[position], &mPairArray[nextPos], sizeof(NodeTimeoutPair) * moveNum);
        }

        mPairArraySize--;
    }

    static void TimerCallbackInterface(System::Layer * aLayer, void * aAppState)
    {
        // Normaly we would call the callback here, thus scheduling an engine run, but we don't need it for this test as we simulate
        // all the callbacks related to report emissions. The actual callback should look like this:
        //
        // ReadHandlerNode * node = static_cast<ReadHandlerNode *>(aAppState);
        // node->RunCallback();
        ChipLogProgress(DataManagement, "Simluating engine run for Handler: %p", aAppState);
    }
    virtual CHIP_ERROR StartTimer(void * context, System::Clock::Timeout aTimeout) override
    {
        return insertPair(static_cast<ReadHandlerNode *>(context), aTimeout + mMockSystemTimestamp);
    }
    virtual void CancelTimer(void * context) override { removePair(static_cast<ReadHandlerNode *>(context)); }
    virtual bool IsTimerActive(void * context) override
    {
        size_t position;
        NodeTimeoutPair * pair = FindPair(static_cast<ReadHandlerNode *>(context), position);
        VerifyOrReturnValue(pair != nullptr, false);

        return pair->timeout > mMockSystemTimestamp;
    }

    virtual System::Clock::Timestamp GetCurrentMonotonicTimestamp() override { return mMockSystemTimestamp; }

    void SetMockSystemTimestamp(System::Clock::Timestamp aMockTimestamp) { mMockSystemTimestamp = aMockTimestamp; }

    // Increment the mock timestamp by aTime and call callbacks for timers that have expired. Checks if the timeout expired after
    // incrementing
    void IncrementMockTimestamp(System::Clock::Milliseconds64 aTime)
    {
        mMockSystemTimestamp = mMockSystemTimestamp + aTime;
        for (size_t i = 0; i < mPairArraySize; i++)
        {
            if (mPairArray[i].timeout <= mMockSystemTimestamp)
            {
                TimerCallbackInterface(nullptr, mPairArray[i].node);
            }
        }
    }
};

/// @brief TestTimerSynchronizedDelegate is a mock of the TimerDelegate interface that allows to control the time without dependency
/// on the system layer. This also simulates the system timer by verifying if the timeout expired when incrementing the mock
/// timestamp. only one timer can be active at a time, which is the one has the earliest timeout.
/// It is used to test the SynchronizedReportSchedulerImpl.
class TestTimerSynchronizedDelegate : public ReportScheduler::TimerDelegate
{
public:
    static void TimerCallbackInterface(System::Layer * aLayer, void * aAppState)
    {
        SynchronizedReportSchedulerImpl * scheduler = static_cast<SynchronizedReportSchedulerImpl *>(aAppState);
        scheduler->ReportTimerCallback();
    }
    virtual CHIP_ERROR StartTimer(void * context, System::Clock::Timeout aTimeout) override
    {
        SynchronizedReportSchedulerImpl * scheduler = static_cast<SynchronizedReportSchedulerImpl *>(context);
        if (nullptr == scheduler)
        {
            return CHIP_ERROR_INCORRECT_STATE;
        }

        mSyncScheduler = scheduler;
        mTimerTimeout  = mMockSystemTimestamp + aTimeout;
        return CHIP_NO_ERROR;
    }
    virtual void CancelTimer(void * context) override
    {
        VerifyOrReturn(nullptr != mSyncScheduler);
        mSyncScheduler = nullptr;
        mTimerTimeout  = System::Clock::Milliseconds64(0x7FFFFFFFFFFFFFFF);
    }
    virtual bool IsTimerActive(void * context) override
    {
        return (nullptr != mSyncScheduler) && (mTimerTimeout > mMockSystemTimestamp);
    }

    virtual System::Clock::Timestamp GetCurrentMonotonicTimestamp() override { return mMockSystemTimestamp; }

    void SetMockSystemTimestamp(System::Clock::Timestamp aMockTimestamp) { mMockSystemTimestamp = aMockTimestamp; }

    // Increment the mock timestamp one milisecond at a time for a total of aTime miliseconds. Checks if the timeout expired when
    // incrementing
    void IncrementMockTimestamp(System::Clock::Milliseconds64 aTime)
    {
        for (System::Clock::Milliseconds64 i = System::Clock::Milliseconds64(0); i < aTime; i++)
        {
            mMockSystemTimestamp++;
            if (mMockSystemTimestamp == mTimerTimeout)
            {
                TimerCallbackInterface(nullptr, mSyncScheduler);
            }
        }

        if (aTime == System::Clock::Milliseconds64(0))
        {
            if (mMockSystemTimestamp == mTimerTimeout)
            {
                TimerCallbackInterface(nullptr, mSyncScheduler);
            }
        }
    }

    SynchronizedReportSchedulerImpl * mSyncScheduler = nullptr;
    System::Clock::Timeout mTimerTimeout             = System::Clock::Milliseconds64(0x7FFFFFFFFFFFFFFF);
    System::Clock::Timestamp mMockSystemTimestamp    = System::Clock::Milliseconds64(0);
};

TestTimerDelegate sTestTimerDelegate;
ReportSchedulerImpl sScheduler(&sTestTimerDelegate);

TestTimerSynchronizedDelegate sTestTimerSynchronizedDelegate;
SynchronizedReportSchedulerImpl syncScheduler(&sTestTimerSynchronizedDelegate);

class TestReportScheduler
{
public:
    static void TestReadHandlerList(nlTestSuite * aSuite, void * aContext)
    {
        TestContext & ctx = *static_cast<TestContext *>(aContext);
        NullReadHandlerCallback nullCallback;
        // exchange context
        Messaging::ExchangeContext * exchangeCtx = ctx.NewExchangeToAlice(nullptr, false);

        // Read handler pool
        ObjectPool<ReadHandler, kNumMaxReadHandlers> readHandlerPool;

        // Initialize mock timestamp
        sTestTimerDelegate.SetMockSystemTimestamp(Milliseconds64(0));

        for (size_t i = 0; i < kNumMaxReadHandlers; i++)
        {
            ReadHandler * readHandler =
                readHandlerPool.CreateObject(nullCallback, exchangeCtx, ReadHandler::InteractionType::Subscribe);
            NL_TEST_ASSERT(aSuite, nullptr != readHandler);
            VerifyOrReturn(nullptr != readHandler);
            // Register ReadHandler using callback method
            sScheduler.OnReadHandlerCreated(readHandler);
            NL_TEST_ASSERT(aSuite, nullptr != sScheduler.FindReadHandlerNode(readHandler));
        }

        NL_TEST_ASSERT(aSuite, readHandlerPool.Allocated() == kNumMaxReadHandlers);
        NL_TEST_ASSERT(aSuite, sScheduler.GetNumReadHandlers() == kNumMaxReadHandlers);
        NL_TEST_ASSERT(aSuite, ctx.GetExchangeManager().GetNumActiveExchanges() == 1);

        // Test unregister first ReadHandler
        ReadHandler * firstReadHandler = sScheduler.mReadHandlerList.begin()->GetReadHandler();
        sScheduler.OnReadHandlerDestroyed(firstReadHandler);
        NL_TEST_ASSERT(aSuite, sScheduler.GetNumReadHandlers() == kNumMaxReadHandlers - 1);
        NL_TEST_ASSERT(aSuite, nullptr == sScheduler.FindReadHandlerNode(firstReadHandler));

        // Test unregister middle ReadHandler
        auto iter = sScheduler.mReadHandlerList.begin();
        for (size_t i = 0; i < static_cast<size_t>(kNumMaxReadHandlers / 2); i++)
        {
            iter++;
        }
        ReadHandler * middleReadHandler = iter->GetReadHandler();
        sScheduler.OnReadHandlerDestroyed(middleReadHandler);
        NL_TEST_ASSERT(aSuite, sScheduler.GetNumReadHandlers() == kNumMaxReadHandlers - 2);
        NL_TEST_ASSERT(aSuite, nullptr == sScheduler.FindReadHandlerNode(middleReadHandler));

        // Test unregister last ReadHandler
        iter = sScheduler.mReadHandlerList.end();
        iter--;
        ReadHandler * lastReadHandler = iter->GetReadHandler();
        sScheduler.OnReadHandlerDestroyed(lastReadHandler);
        NL_TEST_ASSERT(aSuite, sScheduler.GetNumReadHandlers() == kNumMaxReadHandlers - 3);
        NL_TEST_ASSERT(aSuite, nullptr == sScheduler.FindReadHandlerNode(lastReadHandler));

        sScheduler.UnregisterAllHandlers();
        // Confirm all ReadHandlers are unregistered from the scheduler
        NL_TEST_ASSERT(aSuite, sScheduler.GetNumReadHandlers() == 0);
        readHandlerPool.ForEachActiveObject([&](ReadHandler * handler) {
            NL_TEST_ASSERT(aSuite, nullptr == sScheduler.FindReadHandlerNode(handler));
            return Loop::Continue;
        });

        readHandlerPool.ReleaseAll();
        exchangeCtx->Close();
        NL_TEST_ASSERT(aSuite, ctx.GetExchangeManager().GetNumActiveExchanges() == 0);
    }

    static void TestReportTiming(nlTestSuite * aSuite, void * aContext)
    {
        TestContext & ctx = *static_cast<TestContext *>(aContext);
        NullReadHandlerCallback nullCallback;
        // exchange context
        Messaging::ExchangeContext * exchangeCtx = ctx.NewExchangeToAlice(nullptr, false);

        // Read handler pool
        ObjectPool<ReadHandler, kNumMaxReadHandlers> readHandlerPool;

        // Initialize mock timestamp
        sTestTimerDelegate.SetMockSystemTimestamp(Milliseconds64(0));

        // Dirty read handler, will be triggered at min interval
        ReadHandler * readHandler1 =
            readHandlerPool.CreateObject(nullCallback, exchangeCtx, ReadHandler::InteractionType::Subscribe);
        NL_TEST_ASSERT(aSuite, CHIP_NO_ERROR == readHandler1->SetMaxReportingInterval(2));
        NL_TEST_ASSERT(aSuite, CHIP_NO_ERROR == readHandler1->SetMinReportingIntervalForTests(1));
        // Do those manually to avoid scheduling an engine run
        readHandler1->mState = ReadHandler::HandlerState::GeneratingReports;
        sScheduler.OnReadHandlerCreated(readHandler1);
        readHandler1->ForceDirtyState();

        // Clean read handler, will be triggered at max interval
        ReadHandler * readHandler2 =
            readHandlerPool.CreateObject(nullCallback, exchangeCtx, ReadHandler::InteractionType::Subscribe);
        NL_TEST_ASSERT(aSuite, CHIP_NO_ERROR == readHandler2->SetMaxReportingInterval(3));
        NL_TEST_ASSERT(aSuite, CHIP_NO_ERROR == readHandler2->SetMinReportingIntervalForTests(0));
        // Do those manually to avoid scheduling an engine run
        readHandler2->mState = ReadHandler::HandlerState::GeneratingReports;
        sScheduler.OnReadHandlerCreated(readHandler2);

        // Clean read handler, will be triggered at max interval, but will be cancelled before
        ReadHandler * readHandler3 =
            readHandlerPool.CreateObject(nullCallback, exchangeCtx, ReadHandler::InteractionType::Subscribe);
        NL_TEST_ASSERT(aSuite, CHIP_NO_ERROR == readHandler3->SetMaxReportingInterval(3));
        NL_TEST_ASSERT(aSuite, CHIP_NO_ERROR == readHandler3->SetMinReportingIntervalForTests(0));
        // Do those manually to avoid scheduling an engine run
        readHandler3->mState = ReadHandler::HandlerState::GeneratingReports;
        sScheduler.OnReadHandlerCreated(readHandler3);

        // Confirms that none of the ReadHandlers are currently reportable
        NL_TEST_ASSERT(aSuite, !sScheduler.IsReportableNow(readHandler1));
        NL_TEST_ASSERT(aSuite, !sScheduler.IsReportableNow(readHandler2));
        NL_TEST_ASSERT(aSuite, !sScheduler.IsReportableNow(readHandler3));

        // Simulate system clock increment
        sTestTimerDelegate.IncrementMockTimestamp(Milliseconds64(1100));

        // Checks that the first ReadHandler is reportable after 1 second since it is dirty and min interval has expired
        NL_TEST_ASSERT(aSuite, sScheduler.IsReportableNow(readHandler1));
        NL_TEST_ASSERT(aSuite, !sScheduler.IsReportableNow(readHandler2));
        NL_TEST_ASSERT(aSuite, !sScheduler.IsReportableNow(readHandler3));

        NL_TEST_ASSERT(aSuite, sScheduler.IsReportScheduled(readHandler3));
        sScheduler.CancelReport(readHandler3);
        NL_TEST_ASSERT(aSuite, !sScheduler.IsReportScheduled(readHandler3));

        // Simulate system clock increment
        sTestTimerDelegate.IncrementMockTimestamp(Milliseconds64(2000));

        // Checks that all ReadHandlers are reportable
        NL_TEST_ASSERT(aSuite, sScheduler.IsReportableNow(readHandler1));
        NL_TEST_ASSERT(aSuite, sScheduler.IsReportableNow(readHandler2));
        // Even if its timer got cancelled, readHandler3 should still be considered reportable as the max interval has expired
        // and it is in generating report state
        NL_TEST_ASSERT(aSuite, sScheduler.IsReportableNow(readHandler3));

        sScheduler.UnregisterAllHandlers();
        readHandlerPool.ReleaseAll();
        exchangeCtx->Close();
        NL_TEST_ASSERT(aSuite, ctx.GetExchangeManager().GetNumActiveExchanges() == 0);
    }

    static void TestObserverCallbacks(nlTestSuite * aSuite, void * aContext)
    {
        TestContext & ctx = *static_cast<TestContext *>(aContext);
        NullReadHandlerCallback nullCallback;
        // exchange context
        Messaging::ExchangeContext * exchangeCtx = ctx.NewExchangeToAlice(nullptr, false);

        // Read handler pool
        ObjectPool<ReadHandler, kNumMaxReadHandlers> readHandlerPool;

        // Initialize mock timestamp
        sTestTimerDelegate.SetMockSystemTimestamp(Milliseconds64(0));

        ReadHandler * readHandler =
            readHandlerPool.CreateObject(nullCallback, exchangeCtx, ReadHandler::InteractionType::Subscribe);
        NL_TEST_ASSERT(aSuite, CHIP_NO_ERROR == readHandler->SetMaxReportingInterval(2));
        NL_TEST_ASSERT(aSuite, CHIP_NO_ERROR == readHandler->SetMinReportingIntervalForTests(1));
        // Do those manually to avoid scheduling an engine run
        readHandler->mState = ReadHandler::HandlerState::GeneratingReports;
        readHandler->SetObserver(&sScheduler);

        // Test OnReadHandlerCreated
        readHandler->mObserver->OnReadHandlerCreated(readHandler);
        // Should have registered the read handler in the scheduler and scheduled a report
        NL_TEST_ASSERT(aSuite, sScheduler.GetNumReadHandlers() == 1);
        NL_TEST_ASSERT(aSuite, sScheduler.IsReportScheduled(readHandler));
        ReadHandlerNode * node = sScheduler.FindReadHandlerNode(readHandler);
        NL_TEST_ASSERT(aSuite, nullptr != node);
        VerifyOrReturn(nullptr != node);
        NL_TEST_ASSERT(aSuite, node->GetReadHandler() == readHandler);

        // Test OnBecameReportable
        readHandler->ForceDirtyState();
        readHandler->mObserver->OnBecameReportable(readHandler);
        // Should have changed the scheduled timeout to the handler's min interval, to check, we wait for the min interval to
        // expire
        // Simulate system clock increment
        sTestTimerDelegate.IncrementMockTimestamp(Milliseconds64(1100));

        // Check that no report is scheduled since the min interval has expired, the timer should now be stopped
        NL_TEST_ASSERT(aSuite, !sScheduler.IsReportScheduled(readHandler));

        // Test OnSubscriptionAction
        readHandler->ClearForceDirtyFlag();
        readHandler->mObserver->OnSubscriptionAction(readHandler);
        // Should have changed the scheduled timeout to the handlers max interval, to check, we wait for the min interval to
        // confirm it is not expired yet so the report should still be scheduled

        NL_TEST_ASSERT(aSuite, sScheduler.IsReportScheduled(readHandler));
        // Simulate system clock increment
        sTestTimerDelegate.IncrementMockTimestamp(Milliseconds64(1100));

        // Check that the report is still scheduled as the max interval has not expired yet and the dirty flag was cleared
        NL_TEST_ASSERT(aSuite, sScheduler.IsReportScheduled(readHandler));
        // Simulate system clock increment
        sTestTimerDelegate.IncrementMockTimestamp(Milliseconds64(2100));

        // Check that no report is scheduled since the max interval should have expired, the timer should now be stopped
        NL_TEST_ASSERT(aSuite, !sScheduler.IsReportScheduled(readHandler));

        // Test OnReadHandlerDestroyed
        readHandler->mObserver->OnReadHandlerDestroyed(readHandler);
        // Should have unregistered the read handler in the scheduler and cancelled the report
        NL_TEST_ASSERT(aSuite, !sScheduler.IsReportScheduled(readHandler));
        NL_TEST_ASSERT(aSuite, sScheduler.GetNumReadHandlers() == 0);
        NL_TEST_ASSERT(aSuite, nullptr == sScheduler.FindReadHandlerNode(readHandler));

        sScheduler.OnReadHandlerDestroyed(readHandler);
        readHandlerPool.ReleaseAll();
        exchangeCtx->Close();
        NL_TEST_ASSERT(aSuite, ctx.GetExchangeManager().GetNumActiveExchanges() == 0);
    }

    static void TestSynchronizedScheduler(nlTestSuite * aSuite, void * aContext)
    {
        TestContext & ctx = *static_cast<TestContext *>(aContext);
        NullReadHandlerCallback nullCallback;
        // exchange context
        Messaging::ExchangeContext * exchangeCtx = ctx.NewExchangeToAlice(nullptr, false);

        // First test: ReadHandler 2 merge on ReadHandler 1 max interval
        // Read handler pool
        ObjectPool<ReadHandler, kNumMaxReadHandlers> readHandlerPool;

        // Initilaize the mock system time
        sTestTimerSynchronizedDelegate.SetMockSystemTimestamp(System::Clock::Milliseconds64(0));

        ReadHandler * readHandler1 =
            readHandlerPool.CreateObject(nullCallback, exchangeCtx, ReadHandler::InteractionType::Subscribe);
        NL_TEST_ASSERT(aSuite, CHIP_NO_ERROR == readHandler1->SetMaxReportingInterval(2));
        NL_TEST_ASSERT(aSuite, CHIP_NO_ERROR == readHandler1->SetMinReportingIntervalForTests(0));
        readHandler1->MoveToState(ReadHandler::HandlerState::GeneratingReports);
        readHandler1->SetObserver(&syncScheduler);
        readHandler1->mObserver->OnReadHandlerCreated(readHandler1);

        ReadHandler * readHandler2 =
            readHandlerPool.CreateObject(nullCallback, exchangeCtx, ReadHandler::InteractionType::Subscribe);
        NL_TEST_ASSERT(aSuite, CHIP_NO_ERROR == readHandler2->SetMaxReportingInterval(3));
        NL_TEST_ASSERT(aSuite, CHIP_NO_ERROR == readHandler2->SetMinReportingIntervalForTests(1));
        readHandler2->MoveToState(ReadHandler::HandlerState::GeneratingReports);
        readHandler2->SetObserver(&syncScheduler);
        readHandler2->mObserver->OnReadHandlerCreated(readHandler2);

        // Confirm all handler are currently registered in the scheduler
        NL_TEST_ASSERT(aSuite, syncScheduler.GetNumReadHandlers() == 2);

        ReadHandlerNode * node1 = syncScheduler.FindReadHandlerNode(readHandler1);
        ReadHandlerNode * node2 = syncScheduler.FindReadHandlerNode(readHandler2);

        // Confirm that a report emission is scheduled
        NL_TEST_ASSERT(aSuite, syncScheduler.IsReportScheduled());

        // Validates that the lowest max is selected as the common max timestamp
        NL_TEST_ASSERT(aSuite, syncScheduler.mNextMaxTimestamp == node1->GetMaxTimestamp());
        // Validates that the highest reportable min is selected as the common min interval (0 here)
        NL_TEST_ASSERT(aSuite, syncScheduler.mNextMinTimestamp == node1->GetMinTimestamp());
        // Validates that the next report emission is scheduled on the common max timestamp
        NL_TEST_ASSERT(aSuite, syncScheduler.mTestNextReportTimestamp == syncScheduler.mNextMaxTimestamp);

        // Simulate waiting for the max interval to expire (2s)
        sTestTimerSynchronizedDelegate.IncrementMockTimestamp(System::Clock::Milliseconds64(2000));

        // Confirm that both handlers are now reportable since the timer has expired (readHandler1 from its max and readHandler2
        // from its sync)
        NL_TEST_ASSERT(aSuite, syncScheduler.IsReportableNow(readHandler1));
        NL_TEST_ASSERT(aSuite, syncScheduler.IsReportableNow(readHandler2));
        // Confirm timeout has expired and no report is scheduled, an engine run would typically happen here
        NL_TEST_ASSERT(aSuite, !sScheduler.IsReportScheduled(readHandler1));
        NL_TEST_ASSERT(aSuite, !sScheduler.IsReportScheduled(readHandler2));

        // Simulate a report emission for readHandler1
        readHandler1->mObserver->OnSubscriptionAction(readHandler1);
        // Simulate a report emission for readHandler2
        readHandler2->mObserver->OnSubscriptionAction(readHandler2);

        // Validate that the max timestamp for both readhandlers got updated and that the next report emission is scheduled on
        //  the new max timestamp for readhandler1
        NL_TEST_ASSERT(aSuite, node1->GetMaxTimestamp() > sTestTimerSynchronizedDelegate.GetCurrentMonotonicTimestamp());
        NL_TEST_ASSERT(aSuite, syncScheduler.mTestNextReportTimestamp == node1->GetMaxTimestamp());

        // Confirm behavior when a read handler becomes dirty
        readHandler2->ForceDirtyState();
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler2));
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler1));
        // Simulate wait enough for min timestamp of readHandler2 to be reached (1s)
        sTestTimerSynchronizedDelegate.IncrementMockTimestamp(System::Clock::Milliseconds64(1000));

        NL_TEST_ASSERT(aSuite, syncScheduler.IsReportableNow(readHandler2));
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler1));
        readHandler2->mObserver->OnBecameReportable(readHandler2);

        // confirm report scheduled now
        NL_TEST_ASSERT(aSuite,
                       syncScheduler.mTestNextReportTimestamp == sTestTimerSynchronizedDelegate.GetCurrentMonotonicTimestamp());
        // Increment the timestamp by 0 here to trigger an engine run as the mock timer is only calling the timeout callback if we
        // increment the mock timestamp
        sTestTimerSynchronizedDelegate.IncrementMockTimestamp(System::Clock::Milliseconds64(0));

        // since the min interval on readHandler1 is 0, it should also be reportable now by sync mechanism
        NL_TEST_ASSERT(aSuite, syncScheduler.IsReportableNow(readHandler1));
        NL_TEST_ASSERT(aSuite, node1->GetSyncTimestamp() == node2->GetMinTimestamp());

        // Confirm that the next report emission is scheduled on the min timestamp of readHandler2 as it is the highest reportable
        NL_TEST_ASSERT(aSuite, syncScheduler.mTestNextReportTimestamp == node2->GetMinTimestamp());

        // Simulate a report emission for readHandler1
        readHandler1->mObserver->OnSubscriptionAction(readHandler1);
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler1));

        // ReadHandler 2 should still be reportable since it hasn't emitted a report yet
        NL_TEST_ASSERT(aSuite, syncScheduler.IsReportableNow(readHandler2));
        readHandler2->ClearForceDirtyFlag();
        readHandler2->mObserver->OnSubscriptionAction(readHandler2);
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler2));

        // Validate next report scheduled on the max timestamp of readHandler1
        NL_TEST_ASSERT(aSuite, node1->GetMaxTimestamp() > sTestTimerSynchronizedDelegate.GetCurrentMonotonicTimestamp());
        NL_TEST_ASSERT(aSuite, syncScheduler.mTestNextReportTimestamp == node1->GetMaxTimestamp());

        // Simulate readHandler1 becoming dirty after less than 1 seconds
        sTestTimerSynchronizedDelegate.IncrementMockTimestamp(System::Clock::Milliseconds64(900));
        readHandler1->ForceDirtyState();
        NL_TEST_ASSERT(aSuite, syncScheduler.IsReportableNow(readHandler1));
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler2));
        readHandler1->mObserver->OnBecameReportable(readHandler1);

        // Validate next report scheduled on the min timestamp of readHandler1 (readHandler 2 is not currently reportable)
        NL_TEST_ASSERT(aSuite,
                       syncScheduler.mTestNextReportTimestamp == sTestTimerSynchronizedDelegate.GetCurrentMonotonicTimestamp());
        // Simulate a report emission for readHandler1
        readHandler1->ClearForceDirtyFlag();
        readHandler1->mObserver->OnSubscriptionAction(readHandler1);
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler1));
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler2));

        // The next report should be scheduler on the max timestamp of readHandler1 and readHandler2 should be synced
        NL_TEST_ASSERT(aSuite, syncScheduler.mTestNextReportTimestamp == node1->GetMaxTimestamp());
        NL_TEST_ASSERT(aSuite, node2->GetSyncTimestamp() == node1->GetMaxTimestamp());

        sTestTimerSynchronizedDelegate.IncrementMockTimestamp(System::Clock::Milliseconds64(2000));
        NL_TEST_ASSERT(aSuite, syncScheduler.IsReportableNow(readHandler1));
        NL_TEST_ASSERT(aSuite, syncScheduler.IsReportableNow(readHandler2));
        readHandler1->mObserver->OnSubscriptionAction(readHandler1);
        readHandler2->mObserver->OnSubscriptionAction(readHandler2);
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler1));
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler2));
        NL_TEST_ASSERT(aSuite, syncScheduler.mTestNextReportTimestamp == node1->GetMaxTimestamp());

        // Simulate a new ReadHandler being added with a min timestamp that will force a conflict

        // Wait for 1 second, nothing should happen here
        sTestTimerSynchronizedDelegate.IncrementMockTimestamp(System::Clock::Milliseconds64(1000));

        ReadHandler * readHandler3 =
            readHandlerPool.CreateObject(nullCallback, exchangeCtx, ReadHandler::InteractionType::Subscribe);
        NL_TEST_ASSERT(aSuite, CHIP_NO_ERROR == readHandler3->SetMaxReportingInterval(3));
        NL_TEST_ASSERT(aSuite, CHIP_NO_ERROR == readHandler3->SetMinReportingIntervalForTests(2));
        readHandler3->MoveToState(ReadHandler::HandlerState::GeneratingReports);
        readHandler3->SetObserver(&syncScheduler);
        readHandler3->mObserver->OnReadHandlerCreated(readHandler3);

        // Confirm all handler are currently registered in the scheduler
        NL_TEST_ASSERT(aSuite, syncScheduler.GetNumReadHandlers() == 3);
        ReadHandlerNode * node3 = syncScheduler.FindReadHandlerNode(readHandler3);

        // Since the min interval on readHandler3 is 2, it should be above the current max timestamp, therefore the next report
        // should still happen on the max timestamp of readHandler1 and the sync should be done on future reports
        NL_TEST_ASSERT(aSuite, syncScheduler.mTestNextReportTimestamp == node1->GetMaxTimestamp());
        // The min timestamp should also not have changed since the min of readhandler3 is higher than the current max
        NL_TEST_ASSERT(aSuite, syncScheduler.mNextMinTimestamp == node2->GetMinTimestamp());

        sTestTimerSynchronizedDelegate.IncrementMockTimestamp(System::Clock::Milliseconds64(1000));

        // Confirm that readHandler1 and readHandler 2 are now reportable, whilst readHandler3 is not
        NL_TEST_ASSERT(aSuite, syncScheduler.IsReportableNow(readHandler1));
        NL_TEST_ASSERT(aSuite, syncScheduler.IsReportableNow(readHandler2));
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler3));
        readHandler1->mObserver->OnBecameReportable(readHandler1);
        readHandler2->mObserver->OnBecameReportable(readHandler2);

        // Simulate a report emission for readHandler1 and readHandler2
        readHandler1->mObserver->OnSubscriptionAction(readHandler1);
        readHandler1->mObserver->OnSubscriptionAction(readHandler2);
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler1));
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler2));

        // Confirm that next report is scheduled on the max timestamp of readHandler3 and other 2 readHandlers are synced
        NL_TEST_ASSERT(aSuite, syncScheduler.mTestNextReportTimestamp == node3->GetMaxTimestamp());
        NL_TEST_ASSERT(aSuite, node1->GetSyncTimestamp() == node3->GetMaxTimestamp());
        NL_TEST_ASSERT(aSuite, node2->GetSyncTimestamp() == node3->GetMaxTimestamp());

        sTestTimerSynchronizedDelegate.IncrementMockTimestamp(System::Clock::Milliseconds64(2000));

        NL_TEST_ASSERT(aSuite, syncScheduler.IsReportableNow(readHandler1));
        NL_TEST_ASSERT(aSuite, syncScheduler.IsReportableNow(readHandler2));
        NL_TEST_ASSERT(aSuite, syncScheduler.IsReportableNow(readHandler3));
        readHandler1->mObserver->OnBecameReportable(readHandler1);
        readHandler2->mObserver->OnBecameReportable(readHandler2);
        readHandler3->mObserver->OnBecameReportable(readHandler3);
        // Engine run should happen here and send all reports
        readHandler1->mObserver->OnSubscriptionAction(readHandler1);
        readHandler2->mObserver->OnSubscriptionAction(readHandler2);
        readHandler3->mObserver->OnSubscriptionAction(readHandler3);
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler1));
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler2));
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler3));
        NL_TEST_ASSERT(aSuite, syncScheduler.mTestNextReportTimestamp == node1->GetMaxTimestamp());
        NL_TEST_ASSERT(aSuite, node1->GetSyncTimestamp() == node1->GetMaxTimestamp());
        NL_TEST_ASSERT(aSuite, node2->GetSyncTimestamp() == node1->GetMaxTimestamp());

        // Now simulate a new readHandler being added with a max forcing a conflict
        ReadHandler * readHandler4 =
            readHandlerPool.CreateObject(nullCallback, exchangeCtx, ReadHandler::InteractionType::Subscribe);
        NL_TEST_ASSERT(aSuite, CHIP_NO_ERROR == readHandler4->SetMaxReportingInterval(1));
        NL_TEST_ASSERT(aSuite, CHIP_NO_ERROR == readHandler4->SetMinReportingIntervalForTests(0));
        readHandler4->MoveToState(ReadHandler::HandlerState::GeneratingReports);
        readHandler4->SetObserver(&syncScheduler);
        readHandler4->mObserver->OnReadHandlerCreated(readHandler4);

        // Confirm all handler are currently registered in the scheduler
        NL_TEST_ASSERT(aSuite, syncScheduler.GetNumReadHandlers() == 4);
        ReadHandlerNode * node4 = syncScheduler.FindReadHandlerNode(readHandler4);

        // Confirm next report is scheduled on the max timestamp of readHandler4 and other handlers 1 and 2 are synced
        NL_TEST_ASSERT(aSuite, syncScheduler.mTestNextReportTimestamp == node4->GetMaxTimestamp());
        NL_TEST_ASSERT(aSuite, node1->GetSyncTimestamp() == node4->GetMaxTimestamp());
        NL_TEST_ASSERT(aSuite, node2->GetSyncTimestamp() == node4->GetMaxTimestamp());

        // Confirm handler 3 is synched on a later timestamp since its min is higher than the max of readHandler4
        NL_TEST_ASSERT(aSuite, node3->GetSyncTimestamp() == node1->GetMaxTimestamp());

        sTestTimerSynchronizedDelegate.IncrementMockTimestamp(System::Clock::Milliseconds64(1100));

        // Confirm readHandler1, 2 and 4 are reportable
        NL_TEST_ASSERT(aSuite, syncScheduler.IsReportableNow(readHandler1));
        NL_TEST_ASSERT(aSuite, syncScheduler.IsReportableNow(readHandler2));
        NL_TEST_ASSERT(aSuite, syncScheduler.IsReportableNow(readHandler4));

        // Confirm readHandler3 is not reportable
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler3));
        readHandler4->mObserver->OnBecameReportable(readHandler1);
        readHandler4->mObserver->OnBecameReportable(readHandler2);
        readHandler4->mObserver->OnBecameReportable(readHandler4);
        readHandler4->mObserver->OnSubscriptionAction(readHandler1);
        readHandler4->mObserver->OnSubscriptionAction(readHandler2);
        readHandler4->mObserver->OnSubscriptionAction(readHandler4);
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler1));
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler2));
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler4));

        sTestTimerSynchronizedDelegate.IncrementMockTimestamp(System::Clock::Milliseconds64(1000));

        // Confirm  readHandler3 is reportable and other handlers are synced
        NL_TEST_ASSERT(aSuite, syncScheduler.IsReportableNow(readHandler1));
        NL_TEST_ASSERT(aSuite, syncScheduler.IsReportableNow(readHandler2));
        NL_TEST_ASSERT(aSuite, syncScheduler.IsReportableNow(readHandler3));
        NL_TEST_ASSERT(aSuite, syncScheduler.IsReportableNow(readHandler4));
        syncScheduler.OnBecameReportable(readHandler1);
        syncScheduler.OnBecameReportable(readHandler2);
        syncScheduler.OnBecameReportable(readHandler3);
        syncScheduler.OnBecameReportable(readHandler4);
        syncScheduler.OnSubscriptionAction(readHandler1);
        syncScheduler.OnSubscriptionAction(readHandler2);
        syncScheduler.OnSubscriptionAction(readHandler3);
        syncScheduler.OnSubscriptionAction(readHandler4);
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler1));
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler2));
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler3));
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler4));

        // Next emission should be scheduled on the max timestamp of readHandler4 as it is the most restrictive, and handlers 1 and
        // 2 should be synced to handler 4
        NL_TEST_ASSERT(aSuite, syncScheduler.mTestNextReportTimestamp == node4->GetMaxTimestamp());
        NL_TEST_ASSERT(aSuite, node1->GetSyncTimestamp() == node4->GetMaxTimestamp());
        NL_TEST_ASSERT(aSuite, node2->GetSyncTimestamp() == node4->GetMaxTimestamp());
        // handler 3 should have a sync on a different point as its min is higher, in this case it is the max timestamp of handler 1
        NL_TEST_ASSERT(aSuite, node3->GetSyncTimestamp() == node1->GetMaxTimestamp());

        sTestTimerSynchronizedDelegate.IncrementMockTimestamp(System::Clock::Milliseconds64(1000));

        // Confirm  readHandler 1-2-4 are reportable
        NL_TEST_ASSERT(aSuite, syncScheduler.IsReportableNow(readHandler1));
        NL_TEST_ASSERT(aSuite, syncScheduler.IsReportableNow(readHandler2));
        NL_TEST_ASSERT(aSuite, syncScheduler.IsReportableNow(readHandler4));

        // Confirm readHandler3 is not reportable because of its min interval
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler3));

        syncScheduler.OnReadHandlerDestroyed(readHandler1);
        syncScheduler.OnReadHandlerDestroyed(readHandler2);
        syncScheduler.OnReadHandlerDestroyed(readHandler3);
        syncScheduler.OnReadHandlerDestroyed(readHandler4);

        // Reset all handlers
        // Test case: Scheduler 1 and 2 are reportable but min2 > max1, they should sync only when possible (min2 = 3, max1 = 2)
        NL_TEST_ASSERT(aSuite, syncScheduler.GetNumReadHandlers() == 0);

        readHandler1->MoveToState(ReadHandler::HandlerState::Idle);
        NL_TEST_ASSERT(aSuite, CHIP_NO_ERROR == readHandler1->SetMaxReportingInterval(2));
        NL_TEST_ASSERT(aSuite, CHIP_NO_ERROR == readHandler1->SetMinReportingIntervalForTests(0));
        readHandler1->MoveToState(ReadHandler::HandlerState::GeneratingReports);
        syncScheduler.OnReadHandlerCreated(readHandler1);
        readHandler1->ForceDirtyState();
        syncScheduler.OnBecameReportable(readHandler1);
        NL_TEST_ASSERT(aSuite, syncScheduler.IsReportableNow(readHandler1));

        readHandler2->MoveToState(ReadHandler::HandlerState::Idle);
        NL_TEST_ASSERT(aSuite, CHIP_NO_ERROR == readHandler2->SetMaxReportingInterval(4));
        NL_TEST_ASSERT(aSuite, CHIP_NO_ERROR == readHandler2->SetMinReportingIntervalForTests(3));
        readHandler2->MoveToState(ReadHandler::HandlerState::GeneratingReports);
        syncScheduler.OnReadHandlerCreated(readHandler2);
        readHandler2->ForceDirtyState();
        syncScheduler.OnBecameReportable(readHandler2);
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler2));

        node1 = syncScheduler.FindReadHandlerNode(readHandler1);
        node2 = syncScheduler.FindReadHandlerNode(readHandler2);

        // Verify report is scheduled immediately as readHandler1 is dirty and its min == 0
        NL_TEST_ASSERT(aSuite, syncScheduler.mTestNextReportTimestamp == node1->GetMinTimestamp());
        readHandler1->ClearForceDirtyFlag(); // report got emited so clear dirty flag
        syncScheduler.OnSubscriptionAction(readHandler1);
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler1));
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler2));

        // Confirm next report is scheduled on the max timestamp of readHandler1 and readhandler2 is not synced
        NL_TEST_ASSERT(aSuite, syncScheduler.mTestNextReportTimestamp == node1->GetMaxTimestamp());
        // Node 2's sync timestamp should have remained unaffected since its min is higher
        NL_TEST_ASSERT(aSuite, node2->GetSyncTimestamp() == node2->GetMaxTimestamp());

        sTestTimerSynchronizedDelegate.IncrementMockTimestamp(System::Clock::Milliseconds64(2000));
        // Verify handler 1 became reportable
        NL_TEST_ASSERT(aSuite, syncScheduler.IsReportableNow(readHandler1));
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler2));
        syncScheduler.OnBecameReportable(readHandler1);

        // simulate run with only readhandler1 reportable
        syncScheduler.OnSubscriptionAction(readHandler1);
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler1));
        NL_TEST_ASSERT(aSuite, !syncScheduler.IsReportableNow(readHandler2));
        NL_TEST_ASSERT(aSuite, syncScheduler.mTestNextReportTimestamp == node2->GetMinTimestamp());
        NL_TEST_ASSERT(aSuite, node1->GetSyncTimestamp() == node2->GetMinTimestamp());

        sTestTimerSynchronizedDelegate.IncrementMockTimestamp(System::Clock::Milliseconds64(1000));
        NL_TEST_ASSERT(aSuite, syncScheduler.IsReportableNow(readHandler1));
        NL_TEST_ASSERT(aSuite, syncScheduler.IsReportableNow(readHandler2));

        readHandler2->ClearForceDirtyFlag();
        syncScheduler.OnSubscriptionAction(readHandler1);
        syncScheduler.OnSubscriptionAction(readHandler2);

        NL_TEST_ASSERT(aSuite, syncScheduler.mTestNextReportTimestamp == node1->GetMaxTimestamp());
        NL_TEST_ASSERT(aSuite, node2->GetSyncTimestamp() == node2->GetMaxTimestamp());

        syncScheduler.UnregisterAllHandlers();
        readHandlerPool.ReleaseAll();
        exchangeCtx->Close();
        NL_TEST_ASSERT(aSuite, ctx.GetExchangeManager().GetNumActiveExchanges() == 0);
    }
};

} // namespace reporting
} // namespace app
} // namespace chip

namespace {

/**
 *   Test Suite. It lists all the test functions.
 */

static nlTest sTests[] = {
    NL_TEST_DEF("TestReadHandlerList", chip::app::reporting::TestReportScheduler::TestReadHandlerList),
    NL_TEST_DEF("TestReportTiming", chip::app::reporting::TestReportScheduler::TestReportTiming),
    NL_TEST_DEF("TestObserverCallbacks", chip::app::reporting::TestReportScheduler::TestObserverCallbacks),
    NL_TEST_DEF("TestSynchronizedScheduler", chip::app::reporting::TestReportScheduler::TestSynchronizedScheduler),
    NL_TEST_SENTINEL(),
};

nlTestSuite sSuite = { "TestReportScheduler", &sTests[0], TestContext::Initialize, TestContext::Finalize };

} // namespace

int TestReportScheduler()
{
    return chip::ExecuteTestsWithContext<TestContext>(&sSuite);
}

CHIP_REGISTER_TEST_SUITE(TestReportScheduler);
