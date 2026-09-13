#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>

#include "app/FrameQueue.h"

namespace {

using namespace std::chrono_literals;

struct Item {
    explicit Item(int itemValue) : value(itemValue) { ++alive; }
    ~Item() { --alive; }

    int value;
    static std::atomic<int> alive;
};

std::atomic<int> Item::alive{0};

void deleteItem(Item* item) { delete item; }

struct CloseOnExit {
    odyssey::FrameQueue<Item>& queue;
    ~CloseOnExit() { queue.close(); }
};

} // namespace

using odyssey::FrameQueue;

TEST(FrameQueue, TakesPublishedFramesInOrder) {
    FrameQueue<Item> queue(3, &deleteItem);
    ASSERT_TRUE(queue.publish(new Item(1)));
    ASSERT_TRUE(queue.publish(new Item(2)));
    ASSERT_TRUE(queue.publish(new Item(3)));

    for (int expected = 1; expected <= 3; ++expected) {
        Item* item = queue.tryTake();
        ASSERT_NE(item, nullptr);
        EXPECT_EQ(item->value, expected);
        delete item;
    }
    EXPECT_EQ(queue.tryTake(), nullptr);
}

TEST(FrameQueue, FullQueueBlocksPublisherUntilConsumerMakesSpace) {
    FrameQueue<Item> queue(1, &deleteItem);
    ASSERT_TRUE(queue.publish(new Item(1)));

    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    auto published = std::async(std::launch::async, [&] {
        entered.set_value();
        return queue.publish(new Item(2));
    });
    CloseOnExit closeOnExit{queue};
    ASSERT_EQ(enteredFuture.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(published.wait_for(20ms), std::future_status::timeout);

    Item* first = queue.tryTake();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->value, 1);
    delete first;

    ASSERT_EQ(published.wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(published.get());
    Item* second = queue.tryTake();
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->value, 2);
    delete second;
}

TEST(FrameQueue, CloseRejectsAndDeletesBlockedPublishButAllowsDrain) {
    Item::alive = 0;
    FrameQueue<Item> queue(1, &deleteItem);
    ASSERT_TRUE(queue.publish(new Item(1)));

    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    auto published = std::async(std::launch::async, [&] {
        entered.set_value();
        return queue.publish(new Item(2));
    });
    CloseOnExit closeOnExit{queue};
    ASSERT_EQ(enteredFuture.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(published.wait_for(20ms), std::future_status::timeout);

    queue.close();
    ASSERT_EQ(published.wait_for(1s), std::future_status::ready);
    EXPECT_FALSE(published.get());
    EXPECT_EQ(Item::alive.load(), 1);

    Item* remaining = queue.tryTake();
    ASSERT_NE(remaining, nullptr);
    EXPECT_EQ(remaining->value, 1);
    delete remaining;
    EXPECT_EQ(Item::alive.load(), 0);

    EXPECT_FALSE(queue.publish(new Item(3)));
    EXPECT_EQ(Item::alive.load(), 0);
}

TEST(FrameQueue, ClearDeletesQueuedFramesAndWakesBlockedPublisher) {
    Item::alive = 0;
    FrameQueue<Item> queue(1, &deleteItem);
    ASSERT_TRUE(queue.publish(new Item(1)));

    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    auto published = std::async(std::launch::async, [&] {
        entered.set_value();
        return queue.publish(new Item(2));
    });
    CloseOnExit closeOnExit{queue};
    ASSERT_EQ(enteredFuture.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(published.wait_for(20ms), std::future_status::timeout);

    queue.clear();
    ASSERT_EQ(published.wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(published.get());
    EXPECT_EQ(Item::alive.load(), 1);

    Item* item = queue.tryTake();
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->value, 2);
    delete item;
    EXPECT_EQ(Item::alive.load(), 0);
}
