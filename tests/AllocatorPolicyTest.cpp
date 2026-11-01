#include "app/AllocatorPolicy.h"

#include <QTest>

#include <cstdlib>
#include <cstring>

// Asserted on glibc's own counters rather than on RSS, which depends on
// everything else the process did.
class AllocatorPolicyTest : public QObject
{
    Q_OBJECT

#if defined(__GLIBC__)
    static size_t mappedBytes() { return mallinfo2().hblkhd; }

    // Whether a block of this size is served by its own mapping, and so goes
    // back to the system at free.
    static bool servedByMmap(size_t bytes)
    {
        const size_t before = mappedBytes();
        // volatile: the compiler may otherwise drop the whole allocation.
        void *volatile block = std::malloc(bytes);
        std::memset(block, 1, bytes);
        const bool mapped = mappedBytes() >= before + bytes;
        std::free(block);
        return mapped && mappedBytes() == before;
    }
#endif

private slots:
    void aFreedBlockDoesNotMoveTheThreshold()
    {
#if defined(__GLIBC__)
        QVERIFY(lightning::applyAllocatorPolicy());
        constexpr size_t kPayload = 4 * 1024 * 1024;
        // Twice: without the policy the first free raises the threshold and
        // the second block lands in the heap.
        QVERIFY(servedByMmap(kPayload));
        QVERIFY(servedByMmap(kPayload));
        QVERIFY(servedByMmap(256 * 1024));
#else
        QSKIP("glibc only");
#endif
    }

    // Frames must come from the heap for the length of a call, and only then.
    void aCallLiftsTheThresholdUntilTheLastOneEnds()
    {
#if defined(__GLIBC__)
        QVERIFY(lightning::applyAllocatorPolicy());
        constexpr size_t kFrame = 1920 * 1080 * 4;
        QVERIFY(servedByMmap(kFrame));
        {
            lightning::CallAllocatorHold first;
            first.engage();
            first.engage(); // idempotent: one call, one count
            QCOMPARE(lightning::CallAllocatorHold::activeCalls(), 1);
            QVERIFY(!servedByMmap(kFrame));
            {
                lightning::CallAllocatorHold second;
                second.engage();
                QCOMPARE(lightning::CallAllocatorHold::activeCalls(), 2);
                second.release();
                second.release();
                QCOMPARE(lightning::CallAllocatorHold::activeCalls(), 1);
                // One call still running.
                QVERIFY(!servedByMmap(kFrame));
            }
        }
        QCOMPARE(lightning::CallAllocatorHold::activeCalls(), 0);
        QVERIFY(servedByMmap(kFrame));
#else
        QSKIP("glibc only");
#endif
    }
};

QTEST_APPLESS_MAIN(AllocatorPolicyTest)
#include "AllocatorPolicyTest.moc"
