#pragma once

#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// A small D-Bus client, limited to what reading media players takes: connecting
// to the session bus and making method calls with at most one string argument.
// https://dbus.freedesktop.org/doc/dbus-specification.html
namespace anisthesia::lin::detail::dbus {

using bytes_t = std::span<const std::uint8_t>;

// Reads marshalled values. Alignment is relative to the start of `data`, which
// must therefore be the start of a message or of a message body.
//
// The data comes from other programs, so every read is checked against the
// bounds. Once a read has failed, the reader stays failed.
class Reader {
public:
  Reader(bytes_t data, std::endian order) : data_{data}, order_{order} {}

  bool Align(std::size_t alignment);

  bool ReadByte(std::uint8_t& value);
  bool ReadUint32(std::uint32_t& value);
  bool ReadUint64(std::uint64_t& value);
  bool ReadString(std::string_view& value);     // `s` and `o`
  bool ReadSignature(std::string_view& value);  // `g`

  // Reads the length of an array and moves to its first element. The returned
  // position is one past the last element.
  bool EnterArray(std::size_t element_alignment, std::size_t& end);

  // Moves past the values that make up `signature`.
  bool Skip(std::string_view signature);

  std::size_t position() const { return position_; }

private:
  bool Fail();
  bool Has(std::size_t size) const;
  bool SkipType(std::string_view signature, std::size_t& index, int depth);

  bytes_t data_;
  std::endian order_;
  std::size_t position_ = 0;
  bool failed_ = false;
};

enum class MessageType : std::uint8_t {
  Invalid = 0,
  MethodCall = 1,
  MethodReturn = 2,
  Error = 3,
  Signal = 4,
};

struct Message {
  MessageType type = MessageType::Invalid;
  std::uint32_t reply_serial = 0;
  std::string_view signature;
  bytes_t body;
  std::endian order = std::endian::little;
};

struct Call {
  std::string destination;
  std::string path;
  std::string interface;
  std::string member;
  std::string argument;  // no argument if empty
};

std::vector<std::uint8_t> MarshalCall(const Call& call, std::uint32_t serial);

// The size of the message at the start of `data`, or 0 if more data is needed
// to tell. There is no way to find the next message after a malformed one, so
// `std::nullopt` means that the stream can no longer be read.
std::optional<std::size_t> MeasureMessage(bytes_t data);

// `data` must be exactly one message, as measured by `MeasureMessage`.
std::optional<Message> ParseMessage(bytes_t data);

struct Address {
  bool abstract = false;
  std::string path;
};

// Transports other than Unix sockets are left out.
std::vector<Address> ParseAddresses(std::string_view addresses);

class Connection {
public:
  Connection() = default;
  ~Connection();

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  bool OpenSession();

  using reply_proc_t = std::function<void(std::size_t index, const Message&)>;

  // Makes all calls before waiting for any reply, so that peers which do not
  // answer time out together rather than one after another. `reply_proc` gets
  // the index of the call along with each reply or error received in time.
  //
  // Returns false if the connection can no longer be used. Replies that have
  // already been passed to `reply_proc` remain valid.
  bool Exchange(std::span<const Call> calls, std::chrono::milliseconds timeout,
                const reply_proc_t& reply_proc);

private:
  using clock_t = std::chrono::steady_clock;

  bool Connect(const Address& address);
  bool Authenticate(clock_t::time_point deadline);
  bool Send(bytes_t data, clock_t::time_point deadline);
  bool Receive(clock_t::time_point deadline);
  bool Wait(short events, clock_t::time_point deadline);
  void Close();

  int socket_ = -1;
  std::uint32_t serial_ = 0;
  std::vector<std::uint8_t> received_;
};

}  // namespace anisthesia::lin::detail::dbus
