#include <stdio.h>
#include <string.h>

#include <array>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        ++g_failures;
        printf("cppcompat: FAIL: %s\n", message);
    }
}

struct Tracker {
    static int live_count;
    static int move_count;

    int value;

    explicit Tracker(int new_value = 0) : value(new_value)
    {
        ++live_count;
    }

    Tracker(const Tracker& other) : value(other.value)
    {
        ++live_count;
    }

    Tracker(Tracker&& other) noexcept : value(other.value)
    {
        other.value = -1;
        ++live_count;
        ++move_count;
    }

    Tracker& operator=(const Tracker& other)
    {
        value = other.value;
        return *this;
    }

    Tracker& operator=(Tracker&& other) noexcept
    {
        value = other.value;
        other.value = -1;
        ++move_count;
        return *this;
    }

    ~Tracker()
    {
        --live_count;
    }
};

int Tracker::live_count = 0;
int Tracker::move_count = 0;

}  // namespace

static_assert(std::is_same_v<std::remove_reference_t<int&>, int>, "remove_reference_t failed");

int main()
{
    std::vector<int> values = {1, 2, 3};
    values.push_back(4);
    check(values.size() == 4, "vector push_back updates size");
    check(values[3] == 4, "vector stores appended values");
    values.reserve(16);
    check(values.capacity() >= 16, "vector reserve grows capacity");

    std::vector<int> moved_values = std::move(values);
    check(moved_values.size() == 4, "vector move constructor keeps elements");
    check(values.empty(), "moved-from vector becomes empty");

    std::string message("hello");
    message += ", ";
    message += "world";
    message.push_back('!');
    check(strcmp(message.c_str(), "hello, world!") == 0, "string append builds c_str");

    std::string moved_message = std::move(message);
    check(strcmp(moved_message.c_str(), "hello, world!") == 0, "string move keeps contents");
    check(message.empty(), "moved-from string becomes empty");

    std::vector<std::string> words;
    words.emplace_back("alpha");
    words.emplace_back("beta");
    check(words.size() == 2, "vector emplace_back works with strings");
    check(words[1] == std::string("beta"), "vector stores string objects");

    std::array<int, 3> coords = {2, 4, 6};
    check(std::size(coords) == 3, "array reports size");
    check(std::data(coords)[2] == 6, "array data view is valid");

    {
        std::vector<Tracker> tracked;
        tracked.emplace_back(1);
        tracked.emplace_back(2);
        tracked.reserve(8);
        check(Tracker::live_count == 2, "vector reallocation preserves live objects");
        tracked.clear();
        check(Tracker::live_count == 0, "vector clear destroys elements");
    }
    check(Tracker::move_count > 0, "vector reallocation used move construction");

    int* heap_value = new int(7);
    check(heap_value != nullptr, "operator new returns memory");
    check(*heap_value == 7, "operator new storage is writable");
    delete heap_value;

    if (g_failures != 0) {
        printf("cppcompat: %d checks failed\n", g_failures);
        return 1;
    }

    puts("cppcompat: all checks passed");
    return 0;
}