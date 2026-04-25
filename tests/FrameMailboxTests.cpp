#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "app/FrameMailbox.h"

namespace {

// Simple integer "frame" used to exercise mailbox semantics without dragging
// FFmpeg in. A counter tracks how many items were deleted so tests can assert
// that dropped publishes actually release ownership.
struct Item {
    int   value{0};
    static std::atomic<int> alive;
    Item(int v) : value(v) { ++alive; }
    ~Item()                 { --alive; }
};
std::atomic<int> Item::alive{0};

void del(Item* i) { delete i; }

} // namespace

using odyssey::FrameMailbox;

TEST(FrameMailbox, PublishingOverwritesUntakenFrameAndDeletesIt) {
    Item::alive = 0;
    FrameMailbox<Item> mb(&del);
    EXPECT_EQ(mb.publish(new Item(1)), 0);
    EXPECT_EQ(Item::alive.load(), 1);
    EXPECT_EQ(mb.publish(new Item(2)), 1);          // drops #1
    EXPECT_EQ(Item::alive.load(), 1);
    EXPECT_EQ(mb.publish(new Item(3)), 1);          // drops #2
    EXPECT_EQ(Item::alive.load(), 1);

    Item* got = mb.tryTake();
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->value, 3);
    delete got;
    EXPECT_EQ(Item::alive.load(), 0);
}

TEST(FrameMailbox, TryTakeReturnsNullWhenEmpty) {
    FrameMailbox<Item> mb(&del);
    EXPECT_EQ(mb.tryTake(), nullptr);
}

TEST(FrameMailbox, CountersTrackPublishesAndDrops) {
    FrameMailbox<Item> mb(&del);
    mb.publish(new Item(1));
    mb.publish(new Item(2));
    mb.publish(new Item(3));
    Item* got = mb.tryTake();
    delete got;
    mb.publish(new Item(4));
    Item* got2 = mb.tryTake();
    delete got2;
    EXPECT_EQ(mb.publishCount(), 4u);
    EXPECT_EQ(mb.dropCount(),    2u); // publishes 2 and 3 dropped before take
}

TEST(FrameMailbox, TakeBlocksUntilPublish) {
    FrameMailbox<Item> mb(&del);
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};

    std::thread t([&]{
        started.store(true);
        Item* got = mb.take();
        ASSERT_NE(got, nullptr);
        EXPECT_EQ(got->value, 42);
        delete got;
        finished.store(true);
    });

    while (!started.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(finished.load()); // still blocked

    mb.publish(new Item(42));
    t.join();
    EXPECT_TRUE(finished.load());
}

TEST(FrameMailbox, CloseUnblocksTakeReturningNull) {
    FrameMailbox<Item> mb(&del);
    std::thread t([&]{
        EXPECT_EQ(mb.take(), nullptr);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    mb.close();
    t.join();
}

TEST(FrameMailbox, DestructorReleasesLingeringItem) {
    Item::alive = 0;
    {
        FrameMailbox<Item> mb(&del);
        mb.publish(new Item(1));
        EXPECT_EQ(Item::alive.load(), 1);
    }
    EXPECT_EQ(Item::alive.load(), 0);
}

TEST(FrameMailbox, ProducerConsumerStressNoLeaks) {
    Item::alive = 0;
    FrameMailbox<Item> mb(&del);
    constexpr int kPublishes = 5000;

    std::thread producer([&]{
        for (int i = 0; i < kPublishes; ++i) mb.publish(new Item(i));
        mb.close();
    });

    std::thread consumer([&]{
        for (;;) {
            Item* got = mb.take();
            if (!got) break;
            delete got;
        }
    });

    producer.join();
    consumer.join();
    EXPECT_EQ(Item::alive.load(), 0);
    EXPECT_EQ(mb.publishCount(), static_cast<unsigned>(kPublishes));
    // Drops = publishes we never delivered = publishes - deliveries. The
    // consumer receives at least one (the final), usually many. We only
    // assert invariant: drops + deliveries_consumed == publishes.
    const unsigned deliveries = mb.publishCount() - mb.dropCount();
    EXPECT_GE(deliveries, 1u);
    EXPECT_LE(deliveries, static_cast<unsigned>(kPublishes));
}
