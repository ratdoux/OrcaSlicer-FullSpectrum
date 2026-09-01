#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <string>
#include <vector>

// Header under test lives in the GUI layer but has no GUI/wx dependencies -
// only nlohmann::json + TBB, both available transitively via libslic3r.
#include "../../src/slic3r/GUI/ProfileLoadUtil.hpp"

using namespace nlohmann;
using namespace Slic3r::GUI;

// Catch2 v2 assertions are NOT thread-safe. The `work` lambdas here run on TBB
// workers, so they only WRITE their slot (never assert); every check runs on
// the main thread after parallel_load_items / load_section returns (both block
// until the parallel pass completes, and load_section's merge loop runs on the
// calling thread).

TEST_CASE("parallel_load_items fills every slot indexed by n", "[ProfileLoadUtil]")
{
    const int n = 64;
    std::atomic<bool> destroy{false};
    auto slots = parallel_load_items(n, destroy, [](int i, json &slot) {
        slot = i;
    });
    REQUIRE((int) slots.size() == n);
    for (int i = 0; i < n; ++i) {
        REQUIRE(slots[i].is_number_integer());
        REQUIRE(slots[i].get<int>() == i);
    }
}

TEST_CASE("parallel_load_items leaves skipped slots null", "[ProfileLoadUtil]")
{
    const int n = 32;
    std::atomic<bool> destroy{false};
    auto slots = parallel_load_items(n, destroy, [](int i, json &slot) {
        if (i % 2 == 0) return;  // even indices skipped (slot stays null)
        slot = i;
    });
    REQUIRE((int) slots.size() == n);
    for (int i = 0; i < n; ++i) {
        if (i % 2 == 0) {
            REQUIRE(slots[i].is_null());
        } else {
            REQUIRE(slots[i].get<int>() == i);
        }
    }
}

TEST_CASE("parallel_load_items handles an empty range", "[ProfileLoadUtil]")
{
    std::atomic<bool> destroy{false};
    auto slots = parallel_load_items(0, destroy, [](int, json &) {
        FAIL("work must not run for an empty range");
    });
    REQUIRE(slots.empty());
}

TEST_CASE("parallel_load_items with destroy already set writes no slot", "[ProfileLoadUtil]")
{
    // destroy is checked before work() each iteration, so a flag that is already
    // true makes every worker bail without writing. This is the dialog-torn-down
    // teardown path.
    const int n = 16;
    std::atomic<bool> destroy{true};
    auto slots = parallel_load_items(n, destroy, [](int i, json &slot) {
        slot = i;
    });
    REQUIRE((int) slots.size() == n);
    for (int i = 0; i < n; ++i)
        REQUIRE(slots[i].is_null());
}

TEST_CASE("load_section merges non-null slots in list order", "[ProfileLoadUtil]")
{
    const int n = 48;
    std::vector<int> collected;
    std::atomic<bool> destroy{false};
    load_section(n, destroy,
        [](int i, json &slot) { slot = i; },
        [&](json &item) { collected.push_back(item.get<int>()); });

    REQUIRE((int) collected.size() == n);
    for (int i = 0; i < n; ++i)
        REQUIRE(collected[i] == i);
}

TEST_CASE("load_section skips null slots and keeps the rest in order", "[ProfileLoadUtil]")
{
    const int n = 40;
    int expected = 0;
    for (int i = 0; i < n; ++i)
        if (i % 3 != 0) ++expected;

    std::vector<int> collected;
    std::atomic<bool> destroy{false};
    load_section(n, destroy,
        [](int i, json &slot) { if (i % 3 != 0) slot = i; },  // drop multiples of 3
        [&](json &item) { collected.push_back(item.get<int>()); });

    REQUIRE((int) collected.size() == expected);
    for (int v : collected)
        REQUIRE(v % 3 != 0);
    for (size_t i = 1; i < collected.size(); ++i)
        REQUIRE(collected[i] > collected[i - 1]);
}

TEST_CASE("load_section performs no merge when destroy is set", "[ProfileLoadUtil]")
{
    std::vector<int> collected;
    std::atomic<bool> destroy{true};
    load_section(16, destroy,
        [](int i, json &slot) { slot = i; },
        [&](json &item) { collected.push_back(item.get<int>()); });
    REQUIRE(collected.empty());
}

TEST_CASE("load_section handles an empty range", "[ProfileLoadUtil]")
{
    std::vector<int> collected;
    std::atomic<bool> destroy{false};
    load_section(0, destroy,
        [](int, json &) { FAIL("work must not run for an empty range"); },
        [&](json &)      { FAIL("merge must not run for an empty range"); });
    REQUIRE(collected.empty());
}

TEST_CASE("worker pattern: malformed JSON drops only itself, no throw", "[ProfileLoadUtil]")
{
    // Mirrors the no-throw contract used in LoadProfileFamily: parse with the
    // exception-free overload, leave the slot null on failure, and load_section
    // skips it - the rest of the family is unaffected and nothing is thrown.
    const std::vector<std::string> inputs = {"1", "2", "{ broken", "3", "4"};
    std::vector<int> collected;
    std::atomic<bool> destroy{false};
    load_section((int) inputs.size(), destroy,
        [&](int i, json &slot) {
            json j = json::parse(inputs[i], nullptr, false);
            if (j.is_discarded()) return;  // malformed -> drop only this item
            slot = j;
        },
        [&](json &item) { collected.push_back(item.get<int>()); });

    std::vector<int> expected = {1, 2, 3, 4};  // index 2 dropped
    REQUIRE(collected == expected);
}
