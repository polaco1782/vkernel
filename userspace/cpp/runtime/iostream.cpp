#include <iostream>

#include <stdio.h>
#include <string.h>

namespace {

auto write_buffer(std::ostream& stream, const char* buffer, int length) -> std::ostream&
{
    if (buffer == nullptr || length <= 0) {
        return stream;
    }

    return stream.write(buffer, static_cast<std::size_t>(length));
}

auto write_signed(std::ostream& stream, long long value) -> std::ostream&
{
    char buffer[32];
    return write_buffer(stream, buffer, snprintf(buffer, sizeof(buffer), "%lld", value));
}

auto write_unsigned(std::ostream& stream, unsigned long long value) -> std::ostream&
{
    char buffer[32];
    return write_buffer(stream, buffer, snprintf(buffer, sizeof(buffer), "%llu", value));
}

auto write_pointer(std::ostream& stream, const void* value) -> std::ostream&
{
    char buffer[32];
    return write_buffer(stream, buffer, snprintf(buffer, sizeof(buffer), "%p", value));
}

}  // namespace

namespace std {

ostream cout(stdout);
ostream cerr(stderr);

auto ostream::put(char ch) -> ostream&
{
    if (stream_ != nullptr) {
        (void)fputc(static_cast<unsigned char>(ch), stream_);
        if (ch == '\n') {
            (void)fflush(stream_);
        }
    }
    return *this;
}

auto ostream::write(const char* data, size_t count) -> ostream&
{
    if (stream_ != nullptr && data != nullptr && count != 0) {
        (void)fwrite(data, 1, count, stream_);
        if (data[count - 1] == '\n') {
            (void)fflush(stream_);
        }
    }
    return *this;
}

void ostream::flush()
{
    if (stream_ != nullptr) {
        (void)fflush(stream_);
    }
}

auto operator<<(ostream& stream, ostream_manipulator manip) -> ostream&
{
    return manip(stream);
}

auto operator<<(ostream& stream, const char* text) -> ostream&
{
    if (text == nullptr) {
        return stream.write("(null)", 6);
    }
    return stream.write(text, strlen(text));
}

auto operator<<(ostream& stream, const string& text) -> ostream&
{
    return stream.write(text.data(), text.size());
}

auto operator<<(ostream& stream, char ch) -> ostream&
{
    return stream.put(ch);
}

auto operator<<(ostream& stream, signed char ch) -> ostream&
{
    return stream.put(static_cast<char>(ch));
}

auto operator<<(ostream& stream, unsigned char ch) -> ostream&
{
    return stream.put(static_cast<char>(ch));
}

auto operator<<(ostream& stream, bool value) -> ostream&
{
    return stream << (value ? "true" : "false");
}

auto operator<<(ostream& stream, short value) -> ostream&
{
    return write_signed(stream, static_cast<long long>(value));
}

auto operator<<(ostream& stream, unsigned short value) -> ostream&
{
    return write_unsigned(stream, static_cast<unsigned long long>(value));
}

auto operator<<(ostream& stream, int value) -> ostream&
{
    return write_signed(stream, static_cast<long long>(value));
}

auto operator<<(ostream& stream, unsigned int value) -> ostream&
{
    return write_unsigned(stream, static_cast<unsigned long long>(value));
}

auto operator<<(ostream& stream, long value) -> ostream&
{
    return write_signed(stream, static_cast<long long>(value));
}

auto operator<<(ostream& stream, unsigned long value) -> ostream&
{
    return write_unsigned(stream, static_cast<unsigned long long>(value));
}

auto operator<<(ostream& stream, long long value) -> ostream&
{
    return write_signed(stream, value);
}

auto operator<<(ostream& stream, unsigned long long value) -> ostream&
{
    return write_unsigned(stream, value);
}

auto operator<<(ostream& stream, const void* value) -> ostream&
{
    return write_pointer(stream, value);
}

auto endl(ostream& stream) -> ostream&
{
    stream.put('\n');
    stream.flush();
    return stream;
}

auto flush(ostream& stream) -> ostream&
{
    stream.flush();
    return stream;
}

}  // namespace std