#include <anisthesia/lin_dbus.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <utility>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace anisthesia::lin::detail::dbus {

namespace {

// Limits from the specification
constexpr std::size_t kMaxMessageSize = 134'217'728;
constexpr std::size_t kMaxArraySize = 67'108'864;
constexpr int kMaxDepth = 64;

constexpr std::size_t kFixedHeaderSize = 16;

constexpr std::uint8_t kFlagNoAutoStart = 0x2;

enum HeaderField : std::uint8_t {
  kFieldPath = 1,
  kFieldInterface = 2,
  kFieldMember = 3,
  kFieldDestination = 6,
  kFieldReplySerial = 5,
  kFieldSignature = 8,
};

constexpr std::size_t GetAlignment(const char type) {
  switch (type) {
    case 'n':
    case 'q':
      return 2;
    case 'i':
    case 'u':
    case 'b':
    case 'h':
    case 's':
    case 'o':
    case 'a':
      return 4;
    case 'x':
    case 't':
    case 'd':
    case '(':
    case '{':
      return 8;
    default:
      return 1;
  }
}

constexpr std::size_t FixedSize(const char type) {
  return type == 'y' ? 1 : GetAlignment(type);
}

constexpr std::size_t AlignUp(const std::size_t value, const std::size_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

// Finds where the single complete type at `index` ends, or returns npos.
std::size_t FindTypeEnd(const std::string_view signature, std::size_t index,
                        const int depth) {
  if (index >= signature.size() || depth > kMaxDepth)
    return std::string_view::npos;

  const char type = signature[index];

  if (type == 'a')
    return FindTypeEnd(signature, index + 1, depth + 1);

  if (type == '(' || type == '{') {
    const char close = type == '(' ? ')' : '}';
    ++index;
    while (index < signature.size() && signature[index] != close) {
      index = FindTypeEnd(signature, index, depth + 1);
      if (index == std::string_view::npos)
        return index;
    }
    return index < signature.size() ? index + 1 : std::string_view::npos;
  }

  return std::string_view{"ybnqiuxtdsogvh"}.find(type) != std::string_view::npos
             ? index + 1
             : std::string_view::npos;
}

std::uint32_t ToUint32(const bytes_t data, const std::size_t position,
                       const std::endian order) {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    const auto byte = order == std::endian::little ? 3 - i : i;
    value = (value << 8) | data[position + byte];
  }
  return value;
}

std::optional<std::endian> ToByteOrder(const std::uint8_t mark) {
  if (mark == 'l')
    return std::endian::little;
  if (mark == 'B')
    return std::endian::big;
  return std::nullopt;
}

class Writer {
public:
  void Align(const std::size_t alignment) {
    data_.resize(AlignUp(data_.size(), alignment));
  }

  void WriteByte(const std::uint8_t value) { data_.push_back(value); }

  void WriteUint32(const std::uint32_t value) {
    Align(4);
    const auto bytes = std::bit_cast<std::array<std::uint8_t, 4>>(value);
    data_.insert(data_.end(), bytes.begin(), bytes.end());
  }

  void WriteString(const std::string_view value) {
    WriteUint32(static_cast<std::uint32_t>(value.size()));
    data_.insert(data_.end(), value.begin(), value.end());
    data_.push_back(0);
  }

  void WriteSignature(const std::string_view value) {
    WriteByte(static_cast<std::uint8_t>(value.size()));
    data_.insert(data_.end(), value.begin(), value.end());
    data_.push_back(0);
  }

  // Every field is a struct of a code and a variant holding a string-like value
  void WriteField(const HeaderField code, const std::string_view type,
                  const std::string_view value) {
    Align(8);
    WriteByte(code);
    WriteSignature(type);
    if (type == "g")
      WriteSignature(value);
    else
      WriteString(value);
  }

  void PutUint32(const std::size_t position, const std::uint32_t value) {
    const auto bytes = std::bit_cast<std::array<std::uint8_t, 4>>(value);
    std::ranges::copy(bytes, data_.begin() + static_cast<std::ptrdiff_t>(position));
  }

  std::size_t size() const { return data_.size(); }
  std::vector<std::uint8_t> Take() { return std::move(data_); }

private:
  std::vector<std::uint8_t> data_;
};

std::optional<std::string> Unescape(const std::string_view value) {
  std::string result;
  result.reserve(value.size());

  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '%') {
      result.push_back(value[i]);
      continue;
    }
    if (i + 2 >= value.size())
      return std::nullopt;
    unsigned int byte = 0;
    const auto end = value.data() + i + 3;
    const auto [ptr, ec] = std::from_chars(value.data() + i + 1, end, byte, 16);
    if (ec != std::errc{} || ptr != end)
      return std::nullopt;
    result.push_back(static_cast<char>(byte));
    i += 2;
  }

  return result;
}

std::optional<Address> ParseAddress(std::string_view entry) {
  constexpr std::string_view kTransport = "unix:";
  if (!entry.starts_with(kTransport))
    return std::nullopt;
  entry.remove_prefix(kTransport.size());

  std::optional<Address> address;

  while (!entry.empty()) {
    const auto comma = entry.find(',');
    const auto pair = entry.substr(0, comma);
    entry = comma == entry.npos ? std::string_view{} : entry.substr(comma + 1);

    const auto equals = pair.find('=');
    if (equals == pair.npos)
      return std::nullopt;

    const auto key = pair.substr(0, equals);
    if (key != "path" && key != "abstract")
      continue;

    auto value = Unescape(pair.substr(equals + 1));
    if (!value || value->empty())
      return std::nullopt;
    address = Address{.abstract = key == "abstract", .path = std::move(*value)};
  }

  return address;
}

std::vector<Address> GetSessionAddresses() {
  if (const auto addresses = std::getenv("DBUS_SESSION_BUS_ADDRESS"))
    return ParseAddresses(addresses);

  // The same fallback as sd-bus, for sessions that use the per-user bus
  if (const auto runtime_dir = std::getenv("XDG_RUNTIME_DIR"))
    return {Address{.path = std::string{runtime_dir} + "/bus"}};

  return {};
}

}  // namespace

////////////////////////////////////////////////////////////////////////////////

bool Reader::Fail() {
  failed_ = true;
  return false;
}

bool Reader::Has(const std::size_t size) const {
  return !failed_ && position_ <= data_.size() && size <= data_.size() - position_;
}

bool Reader::Align(const std::size_t alignment) {
  if (failed_)
    return false;
  const auto aligned = AlignUp(position_, alignment);
  if (aligned > data_.size())
    return Fail();
  position_ = aligned;
  return true;
}

bool Reader::ReadByte(std::uint8_t& value) {
  if (!Has(1))
    return Fail();
  value = data_[position_++];
  return true;
}

bool Reader::ReadUint32(std::uint32_t& value) {
  if (!Align(4) || !Has(4))
    return Fail();
  value = ToUint32(data_, position_, order_);
  position_ += 4;
  return true;
}

bool Reader::ReadUint64(std::uint64_t& value) {
  if (!Align(8) || !Has(8))
    return Fail();
  const std::uint64_t first = ToUint32(data_, position_, order_);
  const std::uint64_t second = ToUint32(data_, position_ + 4, order_);
  value = order_ == std::endian::little ? (second << 32) | first
                                        : (first << 32) | second;
  position_ += 8;
  return true;
}

bool Reader::ReadString(std::string_view& value) {
  std::uint32_t size = 0;
  if (!ReadUint32(size) || !Has(std::size_t{size} + 1) ||
      data_[position_ + size] != 0)
    return Fail();
  value = {reinterpret_cast<const char*>(data_.data() + position_), size};
  position_ += std::size_t{size} + 1;
  return true;
}

bool Reader::ReadSignature(std::string_view& value) {
  std::uint8_t size = 0;
  if (!ReadByte(size) || !Has(std::size_t{size} + 1) ||
      data_[position_ + size] != 0)
    return Fail();
  value = {reinterpret_cast<const char*>(data_.data() + position_), size};
  position_ += std::size_t{size} + 1;
  return true;
}

bool Reader::EnterArray(const std::size_t element_alignment, std::size_t& end) {
  std::uint32_t size = 0;
  if (!ReadUint32(size) || size > kMaxArraySize || !Align(element_alignment) ||
      !Has(size))
    return Fail();
  end = position_ + size;
  return true;
}

bool Reader::Skip(const std::string_view signature) {
  std::size_t index = 0;
  while (index < signature.size()) {
    if (!SkipType(signature, index, 0))
      return Fail();
  }
  return !failed_;
}

bool Reader::SkipType(const std::string_view signature, std::size_t& index,
                      const int depth) {
  if (failed_ || index >= signature.size())
    return Fail();

  const char type = signature[index];

  switch (type) {
    case 'y':
    case 'n':
    case 'q':
    case 'b':
    case 'i':
    case 'u':
    case 'h':
    case 'x':
    case 't':
    case 'd': {
      const std::size_t size = type == 'b' || type == 'h' ? 4 : FixedSize(type);
      if (!Align(size) || !Has(size))
        return Fail();
      position_ += size;
      ++index;
      return true;
    }
    case 's':
    case 'o': {
      std::string_view value;
      if (!ReadString(value))
        return false;
      ++index;
      return true;
    }
    case 'g': {
      std::string_view value;
      if (!ReadSignature(value))
        return false;
      ++index;
      return true;
    }
    case 'v': {
      std::string_view contents;
      if (!ReadSignature(contents))
        return false;
      // A variant holds exactly one complete type
      if (FindTypeEnd(contents, 0, depth + 1) != contents.size())
        return Fail();
      std::size_t contents_index = 0;
      if (!SkipType(contents, contents_index, depth + 1))
        return false;
      ++index;
      return true;
    }
    case 'a': {
      const auto end = FindTypeEnd(signature, index, depth);
      if (end == std::string_view::npos)
        return Fail();
      // The length covers the elements, so there is no need to walk over them
      std::size_t array_end = 0;
      if (!EnterArray(GetAlignment(signature[index + 1]), array_end))
        return false;
      position_ = array_end;
      index = end;
      return true;
    }
    case '(':
    case '{': {
      const auto end = FindTypeEnd(signature, index, depth);
      if (end == std::string_view::npos || !Align(8))
        return Fail();
      for (++index; index < end - 1;) {
        if (!SkipType(signature, index, depth + 1))
          return false;
      }
      index = end;
      return true;
    }
    default:
      return Fail();
  }
}

////////////////////////////////////////////////////////////////////////////////

std::vector<std::uint8_t> MarshalCall(const Call& call, const std::uint32_t serial) {
  constexpr std::uint8_t kByteOrder =
      std::endian::native == std::endian::little ? 'l' : 'B';

  Writer writer;
  writer.WriteByte(kByteOrder);
  writer.WriteByte(static_cast<std::uint8_t>(MessageType::MethodCall));
  writer.WriteByte(kFlagNoAutoStart);
  writer.WriteByte(1);  // protocol version
  writer.WriteUint32(0);  // body size, written below
  writer.WriteUint32(serial);
  writer.WriteUint32(0);  // size of the header fields, written below

  writer.Align(8);
  const auto fields_start = writer.size();
  writer.WriteField(kFieldPath, "o", call.path);
  writer.WriteField(kFieldInterface, "s", call.interface);
  writer.WriteField(kFieldMember, "s", call.member);
  writer.WriteField(kFieldDestination, "s", call.destination);
  if (!call.argument.empty())
    writer.WriteField(kFieldSignature, "g", "s");
  const auto fields_size = writer.size() - fields_start;

  writer.Align(8);
  const auto body_start = writer.size();
  if (!call.argument.empty())
    writer.WriteString(call.argument);
  const auto body_size = writer.size() - body_start;

  writer.PutUint32(4, static_cast<std::uint32_t>(body_size));
  writer.PutUint32(12, static_cast<std::uint32_t>(fields_size));

  return writer.Take();
}

std::optional<std::size_t> MeasureMessage(const bytes_t data) {
  if (data.size() < kFixedHeaderSize)
    return 0;

  const auto order = ToByteOrder(data[0]);
  if (!order || data[3] != 1)
    return std::nullopt;

  const std::size_t body_size = ToUint32(data, 4, *order);
  const std::size_t fields_size = ToUint32(data, 12, *order);
  if (body_size > kMaxMessageSize || fields_size > kMaxArraySize)
    return std::nullopt;

  const auto size = AlignUp(kFixedHeaderSize + fields_size, 8) + body_size;
  if (size > kMaxMessageSize)
    return std::nullopt;
  return size;
}

std::optional<Message> ParseMessage(const bytes_t data) {
  if (data.size() < kFixedHeaderSize)
    return std::nullopt;

  const auto order = ToByteOrder(data[0]);
  if (!order)
    return std::nullopt;

  Message message;
  message.type = static_cast<MessageType>(data[1]);
  message.order = *order;

  const std::size_t body_size = ToUint32(data, 4, *order);

  Reader reader{data, *order};
  std::uint32_t skipped = 0;
  if (!reader.ReadUint32(skipped) || !reader.ReadUint32(skipped) ||
      !reader.ReadUint32(skipped))
    return std::nullopt;

  std::size_t fields_end = 0;
  if (!reader.EnterArray(8, fields_end))
    return std::nullopt;

  while (reader.position() < fields_end) {
    std::uint8_t code = 0;
    std::string_view type;
    if (!reader.Align(8) || !reader.ReadByte(code) || !reader.ReadSignature(type))
      return std::nullopt;

    if (code == kFieldReplySerial && type == "u") {
      if (!reader.ReadUint32(message.reply_serial))
        return std::nullopt;
    } else if (code == kFieldSignature && type == "g") {
      if (!reader.ReadSignature(message.signature))
        return std::nullopt;
    } else if (FindTypeEnd(type, 0, 0) != type.size() || !reader.Skip(type)) {
      return std::nullopt;
    }
  }

  if (reader.position() != fields_end || !reader.Align(8) ||
      data.size() - reader.position() != body_size)
    return std::nullopt;

  message.body = data.subspan(reader.position());
  return message;
}

std::vector<Address> ParseAddresses(std::string_view addresses) {
  std::vector<Address> result;

  while (!addresses.empty()) {
    const auto semicolon = addresses.find(';');
    if (auto address = ParseAddress(addresses.substr(0, semicolon)))
      result.push_back(std::move(*address));
    addresses = semicolon == addresses.npos ? std::string_view{}
                                            : addresses.substr(semicolon + 1);
  }

  return result;
}

////////////////////////////////////////////////////////////////////////////////

Connection::~Connection() {
  Close();
}

void Connection::Close() {
  if (socket_ >= 0)
    ::close(socket_);
  socket_ = -1;
}

bool Connection::Connect(const Address& address) {
  sockaddr_un name{};
  name.sun_family = AF_UNIX;

  const std::size_t offset = address.abstract ? 1 : 0;
  // A path needs its terminator; an abstract name is measured by its length
  if (address.path.size() + offset + (address.abstract ? 0 : 1) >
      sizeof(name.sun_path))
    return false;
  std::memcpy(name.sun_path + offset, address.path.data(), address.path.size());

  const auto size = static_cast<socklen_t>(
      offsetof(sockaddr_un, sun_path) + offset + address.path.size() +
      (address.abstract ? 0 : 1));

  // Non-blocking, so that neither connecting nor a stuck bus can hold up a
  // detection round. A full listen queue fails here and is retried next round.
  socket_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (socket_ < 0)
    return false;

  if (::connect(socket_, reinterpret_cast<const sockaddr*>(&name), size) < 0) {
    Close();
    return false;
  }
  return true;
}

bool Connection::Wait(const short events, const clock_t::time_point deadline) {
  for (;;) {
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
        deadline - clock_t::now());
    if (remaining.count() <= 0)
      return false;

    pollfd descriptor{.fd = socket_, .events = events, .revents = 0};
    const int result = ::poll(&descriptor, 1, static_cast<int>(remaining.count()));
    if (result > 0)
      return true;
    if (result == 0 || errno != EINTR)
      return false;
  }
}

bool Connection::Send(bytes_t data, const clock_t::time_point deadline) {
  while (!data.empty()) {
    // A closed peer must be an error here rather than a SIGPIPE for the program
    const auto sent = ::send(socket_, data.data(), data.size(), MSG_NOSIGNAL);
    if (sent >= 0) {
      data = data.subspan(static_cast<std::size_t>(sent));
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      if (!Wait(POLLOUT, deadline))
        return false;
    } else if (errno != EINTR) {
      return false;
    }
  }
  return true;
}

bool Connection::Receive(const clock_t::time_point deadline) {
  std::array<std::uint8_t, 65536> buffer;

  for (;;) {
    const auto received = ::recv(socket_, buffer.data(), buffer.size(), 0);
    if (received > 0) {
      received_.insert(received_.end(), buffer.begin(), buffer.begin() + received);
      return true;
    }
    if (received == 0)
      return false;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      if (!Wait(POLLIN, deadline))
        return false;
    } else if (errno != EINTR) {
      return false;
    }
  }
}

bool Connection::Authenticate(const clock_t::time_point deadline) {
  constexpr std::string_view kDigits = "0123456789abcdef";

  // The identity is the user ID in decimal, which the protocol then hex-encodes
  std::string identity;
  for (const char c : std::to_string(::geteuid())) {
    identity.push_back(kDigits[static_cast<unsigned char>(c) >> 4]);
    identity.push_back(kDigits[static_cast<unsigned char>(c) & 0xf]);
  }

  std::string request{'\0'};
  request += "AUTH EXTERNAL " + identity + "\r\n";
  if (!Send({reinterpret_cast<const std::uint8_t*>(request.data()), request.size()},
            deadline))
    return false;

  constexpr std::string_view kLineEnd = "\r\n";
  constexpr std::size_t kMaxLineSize = 16384;

  for (;;) {
    const auto end = std::ranges::search(received_, kLineEnd);
    if (end.empty()) {
      if (received_.size() > kMaxLineSize || !Receive(deadline))
        return false;
      continue;
    }

    const bool accepted = received_.size() >= 3 && received_[0] == 'O' &&
                          received_[1] == 'K' && received_[2] == ' ';
    received_.erase(received_.begin(), end.end());
    if (!accepted)
      return false;

    constexpr std::string_view kBegin = "BEGIN\r\n";
    return Send({reinterpret_cast<const std::uint8_t*>(kBegin.data()), kBegin.size()},
                deadline);
  }
}

bool Connection::OpenSession() {
  constexpr std::chrono::seconds kTimeout{1};

  for (const auto& address : GetSessionAddresses()) {
    if (!Connect(address))
      continue;
    if (!Authenticate(clock_t::now() + kTimeout)) {
      Close();
      received_.clear();
      continue;
    }

    const Call hello{.destination = "org.freedesktop.DBus",
                     .path = "/org/freedesktop/DBus",
                     .interface = "org.freedesktop.DBus",
                     .member = "Hello",
                     .argument = {}};
    bool welcomed = false;
    if (Exchange({&hello, 1}, kTimeout,
                 [&welcomed](std::size_t, const Message& reply) {
                   welcomed = reply.type == MessageType::MethodReturn;
                 }) &&
        welcomed)
      return true;

    Close();
    received_.clear();
  }

  return false;
}

bool Connection::Exchange(const std::span<const Call> calls,
                          const std::chrono::milliseconds timeout,
                          const reply_proc_t& reply_proc) {
  if (socket_ < 0)
    return false;

  const auto deadline = clock_t::now() + timeout;

  std::unordered_map<std::uint32_t, std::size_t> waiting;
  std::vector<std::uint8_t> requests;
  for (std::size_t index = 0; index < calls.size(); ++index) {
    waiting.emplace(++serial_, index);
    const auto request = MarshalCall(calls[index], serial_);
    requests.insert(requests.end(), request.begin(), request.end());
  }
  if (!Send(requests, deadline)) {
    Close();
    return false;
  }

  std::size_t consumed = 0;

  while (!waiting.empty()) {
    const auto size = MeasureMessage(bytes_t{received_}.subspan(consumed));
    if (!size) {
      Close();
      return false;
    }

    if (*size == 0 || received_.size() - consumed < *size) {
      received_.erase(received_.begin(),
                      received_.begin() + static_cast<std::ptrdiff_t>(consumed));
      consumed = 0;
      // Running out of time is not the fault of the connection. Those that did
      // not answer are left out, and the bus remains usable for the next call.
      if (clock_t::now() >= deadline)
        return true;
      if (!Receive(deadline)) {
        if (clock_t::now() >= deadline)
          return true;
        Close();
        return false;
      }
      continue;
    }

    const auto message = ParseMessage(bytes_t{received_}.subspan(consumed, *size));
    if (!message) {
      Close();
      return false;
    }
    consumed += *size;

    if (message->type != MessageType::MethodReturn &&
        message->type != MessageType::Error)
      continue;

    const auto call = waiting.find(message->reply_serial);
    if (call == waiting.end())
      continue;
    const auto index = call->second;
    waiting.erase(call);
    reply_proc(index, *message);
  }

  received_.erase(received_.begin(),
                  received_.begin() + static_cast<std::ptrdiff_t>(consumed));
  return true;
}

}  // namespace anisthesia::lin::detail::dbus
