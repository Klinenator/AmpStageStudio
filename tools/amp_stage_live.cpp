#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <cerrno>
#include <cstring>
#include <cstdio>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <portaudio.h>
#ifdef __linux__
#include <pa_linux_alsa.h>
#endif

#include "../amp_profile.h"
#include "../effects/chorus_effect.h"
#include "../effects/compressor_effect.h"
#include "../effects/klon_effect.h"
#include "../effects/plate_reverb_effect.h"
#include "../effects/rat_effect.h"
#include "../effects/tubescreamer_effect.h"
#include "../live_control.h"
#include "../power_stage.h"
#include "../preamp.h"
#include "../preamp_profile.h"
#include "../tube_stage.h"

namespace {
constexpr unsigned long kFramesPerBuffer = 128;
constexpr int kInputChannels = 2;
constexpr int kOutputChannels = 2;
constexpr double kSampleRateHz = 48000.0;

std::atomic<bool> g_running{true};

struct Config {
  std::string effect = "none";
  std::string effects;
  std::string amp_name;
  std::string amp_file;
  std::string preamp_name;
  std::string preamp_file;
  std::string preset = "marshall";
  std::string device_name = "default";
  std::string input_device_name;
  std::string output_device_name;
  std::string control_file;
  std::string http_host = "127.0.0.1";
  std::string alsa_input;
  std::string alsa_output;
  int http_port = 0;
  bool http_only = false;
  std::optional<double> drive_db;
  std::optional<double> level_db;
  std::optional<double> bright_db;
  std::optional<double> bias_trim;
  std::optional<double> bass;
  std::optional<double> mid;
  std::optional<double> treble;
  std::optional<double> presence;
  std::optional<PowerTubeType> power_tube_type;
  std::optional<double> power_drive_db;
  std::optional<double> power_level_db;
  std::optional<double> power_bias_trim;
  double effect_drive = 0.5;
  double effect_tone = 0.5;
  double effect_level_db = 0.0;
  double effect_clean_blend = 0.45;
};

struct RuntimeSettings {
  std::string amp_name;
  std::string preamp_name;
  PreampProfile preamp_profile;
  TubeStageControls stage_controls;
  bool has_power_stage = false;
  PowerTubeType power_tube_type = PowerTubeType::k6V6;
  PowerStageControls power_controls;
  bool compression_enabled = false;
  bool klon_enabled = false;
  bool tubescreamer_enabled = false;
  bool rat_enabled = false;
  bool chorus_enabled = false;
  bool plate_enabled = false;
  KlonControls klon_controls;
  TubeScreamerControls tubescreamer_controls;
  RatControls rat_controls;
  ChorusControls chorus_controls;
  CompressorControls compressor_controls;
  PlateReverbControls plate_controls;
};

struct AppState {
  PreampProcessor preamp;
  PowerStage power_stage;
  ChorusEffect chorus;
  CompressorEffect compressor;
  KlonEffect effect;
  TubeScreamerEffect tubescreamer;
  RatEffect rat;
  PlateReverbEffect plate;
  std::shared_ptr<RuntimeSettings> settings;
  std::string applied_amp_name;
  std::string applied_preamp_name;
  std::string applied_effect_signature;
  bool applied_power_stage_enabled = false;
  PowerTubeType applied_power_tube_type = PowerTubeType::k6V6;
};

std::string ToLower(std::string value);

struct WebDevice {
  std::string value;
  std::string label;
};

struct AudioDeviceListing {
  std::vector<WebDevice> input_devices;
  std::vector<WebDevice> output_devices;
  std::string backend = "portaudio";
};

struct HttpRequest {
  std::string method;
  std::string path;
  std::map<std::string, std::string> headers;
  std::string body;
};

std::string TrimString(std::string value) {
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), not_space));
  value.erase(
      std::find_if(value.rbegin(), value.rend(), not_space).base(),
      value.end());
  return value;
}

std::filesystem::path RepoRootFromArgv0(const char* argv0) {
  std::error_code ec;
  std::filesystem::path binary_path = std::filesystem::absolute(argv0, ec);
  if (ec) {
    binary_path = std::filesystem::current_path();
  }
  binary_path = std::filesystem::weakly_canonical(binary_path, ec);
  if (ec) {
    binary_path = std::filesystem::absolute(argv0);
  }

  std::filesystem::path parent = binary_path.parent_path();
  if (parent.filename() == "build") {
    return parent.parent_path();
  }
  return std::filesystem::current_path();
}

std::map<std::string, std::string> ParseKeyValueFile(
    const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) {
    return {};
  }

  std::map<std::string, std::string> result;
  std::string line;
  while (std::getline(in, line)) {
    const std::size_t comment_pos = line.find('#');
    if (comment_pos != std::string::npos) {
      line.resize(comment_pos);
    }
    line = TrimString(line);
    if (line.empty()) {
      continue;
    }
    const std::size_t equals_pos = line.find('=');
    if (equals_pos == std::string::npos) {
      continue;
    }
    result[TrimString(line.substr(0, equals_pos))] =
        TrimString(line.substr(equals_pos + 1));
  }
  return result;
}

bool WriteKeyValueFile(const std::filesystem::path& path,
                       const std::map<std::string, std::string>& data,
                       std::string* error_out = nullptr) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);

  const std::filesystem::path tmp_path = path.string() + ".tmp";
  std::ofstream out(tmp_path, std::ios::trunc);
  if (!out) {
    if (error_out) {
      *error_out = "Failed to write control file: " + path.string();
    }
    return false;
  }

  for (const auto& [key, value] : data) {
    if (value.empty()) {
      continue;
    }
    out << key << " = " << value << "\n";
  }
  out.close();
  if (!out) {
    if (error_out) {
      *error_out = "Failed to flush control file: " + path.string();
    }
    return false;
  }

  std::filesystem::rename(tmp_path, path, ec);
  if (ec) {
    std::filesystem::remove(path, ec);
    ec.clear();
    std::filesystem::rename(tmp_path, path, ec);
  }
  if (ec) {
    if (error_out) {
      *error_out = "Failed to replace control file: " + path.string();
    }
    return false;
  }
  return true;
}

std::vector<std::string> ListProfiles(const std::filesystem::path& dir,
                                      const std::string& extension) {
  std::vector<std::string> names;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto path = entry.path();
    if (path.extension() == extension) {
      names.push_back(path.stem().string());
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::map<std::string, std::string> DefaultControlSchema() {
  return {
      {"panel_title", "Preamp"},
      {"drive_label", "Drive"},
      {"level_label", "Level"},
      {"bright_label", "Bright"},
      {"bias_label", "Bias"},
      {"bass_label", ""},
      {"mid_label", ""},
      {"treble_label", ""},
      {"presence_label", ""},
      {"input_hpf_hz", "60.0"},
      {"note",
       "These controls follow the current preamp family. Tone stack behavior is circuit-inspired, not an exact schematic solve."},
  };
}

std::optional<std::string> ResolveSelectedPreampName(
    const std::map<std::string, std::string>& state,
    const std::filesystem::path& repo_root) {
  auto preamp_it = state.find("preamp");
  if (preamp_it != state.end() && !preamp_it->second.empty()) {
    return preamp_it->second;
  }

  auto amp_it = state.find("amp");
  if (amp_it == state.end() || amp_it->second.empty()) {
    return std::nullopt;
  }

  const auto amp_profile =
      ParseKeyValueFile(repo_root / "amps" / (amp_it->second + ".amp"));
  auto preamp_name_it = amp_profile.find("preamp_name");
  if (preamp_name_it == amp_profile.end() || preamp_name_it->second.empty()) {
    return std::nullopt;
  }
  return preamp_name_it->second;
}

std::map<std::string, std::string> LoadControlSchema(
    const std::map<std::string, std::string>& state,
    const std::filesystem::path& repo_root) {
  auto schema = DefaultControlSchema();
  const auto preamp_name = ResolveSelectedPreampName(state, repo_root);
  if (!preamp_name) {
    return schema;
  }

  const auto profile =
      ParseKeyValueFile(repo_root / "preamps" / (*preamp_name + ".preamp"));
  if (profile.empty()) {
    return schema;
  }

  const auto lookup = [&profile](const std::string& key,
                                 const std::string& fallback = std::string()) {
    const auto it = profile.find(key);
    if (it == profile.end()) {
      return fallback;
    }
    return it->second;
  };

  schema["panel_title"] = lookup("name", *preamp_name);
  schema["drive_label"] = lookup("ui_drive_label", schema["drive_label"]);
  schema["level_label"] = lookup("ui_level_label", schema["level_label"]);
  schema["bright_label"] = lookup("ui_bright_label", schema["bright_label"]);
  schema["bias_label"] = lookup("ui_bias_label", schema["bias_label"]);
  schema["bass_label"] = lookup("ui_bass_label");
  schema["mid_label"] = lookup("ui_mid_label");
  schema["treble_label"] = lookup("ui_treble_label");
  schema["presence_label"] = lookup("ui_presence_label");
  schema["input_hpf_hz"] = lookup("input_hpf_hz", schema["input_hpf_hz"]);
  return schema;
}

std::string JsonEscape(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (unsigned char c : value) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buffer[7];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
          out += buffer;
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  return out;
}

std::string JsonObjectFromMap(const std::map<std::string, std::string>& data) {
  std::ostringstream out;
  out << "{";
  bool first = true;
  for (const auto& [key, value] : data) {
    if (!first) {
      out << ",";
    }
    first = false;
    out << "\"" << JsonEscape(key) << "\":"
        << "\"" << JsonEscape(value) << "\"";
  }
  out << "}";
  return out.str();
}

std::string JsonArrayFromStrings(const std::string& key,
                                 const std::vector<std::string>& values) {
  std::ostringstream out;
  out << "{\"" << JsonEscape(key) << "\":[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      out << ",";
    }
    out << "\"" << JsonEscape(values[i]) << "\"";
  }
  out << "]}";
  return out.str();
}

std::string JsonAudioDevices(const AudioDeviceListing& listing) {
  auto write_device_array = [](std::ostringstream& out,
                               const char* key,
                               const std::vector<WebDevice>& devices) {
    out << "\"" << key << "\":[";
    for (std::size_t i = 0; i < devices.size(); ++i) {
      if (i > 0) {
        out << ",";
      }
      out << "{\"value\":\"" << JsonEscape(devices[i].value)
          << "\",\"label\":\"" << JsonEscape(devices[i].label) << "\"}";
    }
    out << "]";
  };

  std::ostringstream out;
  out << "{";
  write_device_array(out, "input_devices", listing.input_devices);
  out << ",";
  write_device_array(out, "output_devices", listing.output_devices);
  out << ",\"backend\":\"" << JsonEscape(listing.backend) << "\"}";
  return out.str();
}

bool ParseJsonString(const std::string& text,
                     std::size_t& pos,
                     std::string& out) {
  if (pos >= text.size() || text[pos] != '"') {
    return false;
  }
  ++pos;
  out.clear();
  while (pos < text.size()) {
    const char c = text[pos++];
    if (c == '"') {
      return true;
    }
    if (c != '\\') {
      out.push_back(c);
      continue;
    }
    if (pos >= text.size()) {
      return false;
    }
    const char escaped = text[pos++];
    switch (escaped) {
      case '"': out.push_back('"'); break;
      case '\\': out.push_back('\\'); break;
      case '/': out.push_back('/'); break;
      case 'b': out.push_back('\b'); break;
      case 'f': out.push_back('\f'); break;
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      case 't': out.push_back('\t'); break;
      default: out.push_back(escaped); break;
    }
  }
  return false;
}

bool ParseFlatJsonObject(const std::string& body,
                         std::map<std::string, std::string>& out) {
  std::size_t pos = 0;
  auto skip_space = [&]() {
    while (pos < body.size() &&
           std::isspace(static_cast<unsigned char>(body[pos]))) {
      ++pos;
    }
  };

  skip_space();
  if (pos >= body.size() || body[pos] != '{') {
    return false;
  }
  ++pos;
  skip_space();
  out.clear();
  if (pos < body.size() && body[pos] == '}') {
    return true;
  }

  while (pos < body.size()) {
    std::string key;
    if (!ParseJsonString(body, pos, key)) {
      return false;
    }
    skip_space();
    if (pos >= body.size() || body[pos] != ':') {
      return false;
    }
    ++pos;
    skip_space();

    std::string value;
    if (pos < body.size() && body[pos] == '"') {
      if (!ParseJsonString(body, pos, value)) {
        return false;
      }
    } else {
      const std::size_t start = pos;
      while (pos < body.size() && body[pos] != ',' && body[pos] != '}') {
        ++pos;
      }
      value = TrimString(body.substr(start, pos - start));
    }
    out[key] = value;

    skip_space();
    if (pos >= body.size()) {
      return false;
    }
    if (body[pos] == '}') {
      return true;
    }
    if (body[pos] != ',') {
      return false;
    }
    ++pos;
    skip_space();
  }
  return false;
}

std::string MimeTypeForPath(const std::filesystem::path& path) {
  if (path.extension() == ".html") {
    return "text/html; charset=utf-8";
  }
  if (path.extension() == ".js") {
    return "application/javascript; charset=utf-8";
  }
  if (path.extension() == ".css") {
    return "text/css; charset=utf-8";
  }
  return "application/octet-stream";
}

std::optional<std::string> ReadTextCommandOutput(const std::string& command) {
  FILE* pipe = popen(command.c_str(), "r");
  if (!pipe) {
    return std::nullopt;
  }

  std::string output;
  std::array<char, 512> buffer{};
  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
    output += buffer.data();
  }
  const int status = pclose(pipe);
  if (status != 0) {
    return std::nullopt;
  }
  return output;
}

std::vector<WebDevice> ListAlsaDevices(const std::string& command) {
  const auto output = ReadTextCommandOutput(command + " -l");
  if (!output) {
    return {};
  }

  const std::regex pattern(
      R"(^card\s+(\d+):\s+([^\s]+)\s+\[(.*?)\],\s+device\s+(\d+):\s+(.*)$)");
  std::vector<WebDevice> devices;
  std::istringstream in(*output);
  std::string line;
  while (std::getline(in, line)) {
    std::smatch match;
    const std::string trimmed = TrimString(line);
    if (!std::regex_match(trimmed, match, pattern)) {
      continue;
    }
    WebDevice device;
    device.value = "plughw:" + match.str(1) + "," + match.str(4);
    device.label = device.value + " - " + match.str(3) + " (" + match.str(2) + ")";
    if (!match.str(5).empty()) {
      device.label += " - " + match.str(5);
    }
    devices.push_back(device);
  }
  return devices;
}

AudioDeviceListing ListAudioDevices() {
  AudioDeviceListing listing;
#ifdef __linux__
  listing.backend = "alsa";
  listing.input_devices = ListAlsaDevices("arecord");
  listing.output_devices = ListAlsaDevices("aplay");
#else
  listing.backend = "portaudio";
  const int count = Pa_GetDeviceCount();
  for (int i = 0; i < count; ++i) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
    if (!info) {
      continue;
    }
    WebDevice device;
    device.value = info->name ? info->name : "";
    device.label = device.value;
    if (info->maxInputChannels >= kInputChannels) {
      listing.input_devices.push_back(device);
    }
    if (info->maxOutputChannels >= kOutputChannels) {
      listing.output_devices.push_back(device);
    }
  }
#endif
  return listing;
}

class EmbeddedHttpServer {
public:
  EmbeddedHttpServer(std::filesystem::path repo_root,
                     std::filesystem::path control_path,
                     std::string host,
                     int port,
                     std::mutex* control_file_mutex)
      : repo_root_(std::move(repo_root)),
        static_dir_(repo_root_ / "web" / "static"),
        control_path_(std::move(control_path)),
        host_(std::move(host)),
        port_(port),
        control_file_mutex_(control_file_mutex) {}

  ~EmbeddedHttpServer() {
    Stop();
  }

  bool Start() {
    if (port_ <= 0) {
      return true;
    }

    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo* result = nullptr;
    const std::string port_string = std::to_string(port_);
    const int gai = getaddrinfo(host_.c_str(), port_string.c_str(), &hints, &result);
    if (gai != 0) {
      std::cerr << "HTTP server getaddrinfo failed: " << gai_strerror(gai) << "\n";
      return false;
    }

    for (struct addrinfo* candidate = result; candidate; candidate = candidate->ai_next) {
      const int fd = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
      if (fd < 0) {
        continue;
      }
      int reuse = 1;
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
      if (::bind(fd, candidate->ai_addr, candidate->ai_addrlen) == 0 &&
          ::listen(fd, 16) == 0) {
        listen_fd_ = fd;
        break;
      }
      ::close(fd);
    }
    freeaddrinfo(result);

    if (listen_fd_ < 0) {
      std::cerr << "Failed to bind HTTP server on " << host_ << ":" << port_ << "\n";
      return false;
    }

    running_ = true;
    thread_ = std::thread([this]() { Run(); });
    return true;
  }

  void Stop() {
    if (!running_) {
      return;
    }
    running_ = false;
    if (listen_fd_ >= 0) {
      ::shutdown(listen_fd_, SHUT_RDWR);
      ::close(listen_fd_);
      listen_fd_ = -1;
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

private:
  void Run() {
    while (running_) {
      fd_set readfds;
      FD_ZERO(&readfds);
      FD_SET(listen_fd_, &readfds);
      struct timeval timeout {};
      timeout.tv_sec = 0;
      timeout.tv_usec = 200000;
      const int ready = select(listen_fd_ + 1, &readfds, nullptr, nullptr, &timeout);
      if (ready <= 0) {
        continue;
      }
      sockaddr_storage client_addr {};
      socklen_t client_len = sizeof(client_addr);
      const int client_fd =
          accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
      if (client_fd < 0) {
        continue;
      }
      HandleClient(client_fd);
    }
  }

  void HandleClient(int client_fd) {
    HttpRequest request;
    if (!ReadRequest(client_fd, request)) {
      SendResponse(client_fd, 400, "Bad Request", "text/plain; charset=utf-8",
                   "Bad Request");
      ::close(client_fd);
      return;
    }

    if (request.method == "GET") {
      HandleGet(client_fd, request.path);
    } else if (request.method == "POST") {
      HandlePost(client_fd, request.path, request.body);
    } else {
      SendResponse(client_fd, 405, "Method Not Allowed",
                   "text/plain; charset=utf-8", "Method Not Allowed");
    }

    ::close(client_fd);
  }

  bool ReadRequest(int client_fd, HttpRequest& request) {
    std::string raw;
    std::array<char, 4096> buffer{};
    std::size_t header_end = std::string::npos;
    while ((header_end = raw.find("\r\n\r\n")) == std::string::npos) {
      const ssize_t bytes = recv(client_fd, buffer.data(), buffer.size(), 0);
      if (bytes <= 0) {
        return false;
      }
      raw.append(buffer.data(), static_cast<std::size_t>(bytes));
      if (raw.size() > 1 << 20) {
        return false;
      }
    }

    const std::size_t body_start = header_end + 4;
    std::istringstream headers(raw.substr(0, header_end));
    std::string request_line;
    if (!std::getline(headers, request_line)) {
      return false;
    }
    if (!request_line.empty() && request_line.back() == '\r') {
      request_line.pop_back();
    }

    std::istringstream request_line_stream(request_line);
    if (!(request_line_stream >> request.method >> request.path)) {
      return false;
    }

    std::string line;
    std::size_t content_length = 0;
    while (std::getline(headers, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      const std::size_t colon = line.find(':');
      if (colon == std::string::npos) {
        continue;
      }
      const std::string key = ToLower(TrimString(line.substr(0, colon)));
      const std::string value = TrimString(line.substr(colon + 1));
      request.headers[key] = value;
      if (key == "content-length") {
        content_length = static_cast<std::size_t>(std::strtoull(value.c_str(), nullptr, 10));
      }
    }

    request.body = raw.substr(body_start);
    while (request.body.size() < content_length) {
      const ssize_t bytes = recv(client_fd, buffer.data(), buffer.size(), 0);
      if (bytes <= 0) {
        return false;
      }
      request.body.append(buffer.data(), static_cast<std::size_t>(bytes));
    }
    if (request.body.size() > content_length) {
      request.body.resize(content_length);
    }

    const std::size_t query_pos = request.path.find('?');
    if (query_pos != std::string::npos) {
      request.path.resize(query_pos);
    }
    return true;
  }

  void HandleGet(int client_fd, const std::string& path) {
    if (path == "/api/state") {
      auto state = LoadStateMap();
      state["_control_file"] = control_path_.string();
      SendJson(client_fd, JsonObjectFromMap(state));
      return;
    }
    if (path == "/api/amps") {
      SendJson(client_fd,
               JsonArrayFromStrings("amps", ListProfiles(repo_root_ / "amps", ".amp")));
      return;
    }
    if (path == "/api/preamps") {
      SendJson(client_fd,
               JsonArrayFromStrings("preamps", ListProfiles(repo_root_ / "preamps", ".preamp")));
      return;
    }
    if (path == "/api/power-tubes") {
      SendJson(client_fd, JsonArrayFromStrings("power_tubes",
                                               {"6V6", "6L6", "EL34", "EL84"}));
      return;
    }
    if (path == "/api/audio-devices") {
      SendJson(client_fd, JsonAudioDevices(ListAudioDevices()));
      return;
    }
    if (path == "/api/control-schema") {
      const auto state = LoadStateMap();
      SendJson(client_fd, JsonObjectFromMap(LoadControlSchema(state, repo_root_)));
      return;
    }
    if (path == "/api/health") {
      SendJson(client_fd, "{\"ok\":true}");
      return;
    }

    std::filesystem::path file_path;
    if (path == "/" || path == "/index.html") {
      file_path = static_dir_ / "index.html";
    } else if (path == "/app.js") {
      file_path = static_dir_ / "app.js";
    } else if (path == "/styles.css") {
      file_path = static_dir_ / "styles.css";
    } else {
      SendResponse(client_fd, 404, "Not Found", "text/plain; charset=utf-8", "Not Found");
      return;
    }

    std::ifstream in(file_path, std::ios::binary);
    if (!in) {
      SendResponse(client_fd, 404, "Not Found", "text/plain; charset=utf-8", "Not Found");
      return;
    }
    std::ostringstream data;
    data << in.rdbuf();
    SendResponse(client_fd, 200, "OK", MimeTypeForPath(file_path), data.str());
  }

  void HandlePost(int client_fd,
                  const std::string& path,
                  const std::string& body) {
    if (path != "/api/state") {
      SendResponse(client_fd, 404, "Not Found", "text/plain; charset=utf-8", "Not Found");
      return;
    }

    std::map<std::string, std::string> updates;
    if (!ParseFlatJsonObject(body, updates)) {
      SendResponse(client_fd, 400, "Bad Request",
                   "text/plain; charset=utf-8", "Invalid JSON");
      return;
    }

    auto current = LoadStateMap();
    for (const auto& [key, value] : updates) {
      if (!key.empty() && key[0] == '_') {
        continue;
      }
      current[key] = value;
    }

    std::string error;
    if (!SaveStateMap(current, &error)) {
      SendResponse(client_fd, 500, "Internal Server Error",
                   "text/plain; charset=utf-8", error);
      return;
    }
    current["_control_file"] = control_path_.string();
    SendJson(client_fd, JsonObjectFromMap(current));
  }

  std::map<std::string, std::string> LoadStateMap() {
    std::scoped_lock lock(*control_file_mutex_);
    return ParseKeyValueFile(control_path_);
  }

  bool SaveStateMap(const std::map<std::string, std::string>& data,
                    std::string* error_out) {
    std::scoped_lock lock(*control_file_mutex_);
    return WriteKeyValueFile(control_path_, data, error_out);
  }

  void SendJson(int client_fd, const std::string& body) {
    SendResponse(client_fd, 200, "OK", "application/json; charset=utf-8", body);
  }

  void SendResponse(int client_fd,
                    int status_code,
                    const char* status_text,
                    const std::string& content_type,
                    const std::string& body) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status_code << " " << status_text << "\r\n"
             << "Content-Type: " << content_type << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n"
             << "Pragma: no-cache\r\n"
             << "Connection: close\r\n\r\n"
             << body;
    const std::string text = response.str();
    send(client_fd, text.data(), text.size(), 0);
  }

  std::filesystem::path repo_root_;
  std::filesystem::path static_dir_;
  std::filesystem::path control_path_;
  std::string host_;
  int port_ = 0;
  int listen_fd_ = -1;
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::mutex* control_file_mutex_ = nullptr;
};

void OnSignal(int) {
  g_running = false;
}

void PrintUsage(const char* program_name) {
  std::cerr
      << "usage: " << program_name << " [options]\n"
      << "  --amp NAME               Amp profile from ./amps, e.g. marshall_jtm45\n"
      << "  --amp-file PATH          Explicit amp profile file path\n"
      << "  --preamp NAME            Preamp profile from ./preamps\n"
      << "  --preamp-file PATH       Explicit preamp profile file path\n"
      << "  --power-tube NAME        6V6, 6L6, EL34, or EL84\n"
      << "  --preset NAME            marshall or fender\n"
      << "  --effect NAME            none, klon, tubescreamer, rat, chorus, compression, or plate\n"
      << "  --effects LIST           Comma-separated chain, e.g. compression,rat,chorus,plate\n"
      << "  --device NAME            Duplex PortAudio device substring, default default\n"
      << "  --input-device NAME      PortAudio input device substring\n"
      << "  --output-device NAME     PortAudio output device substring\n"
      << "  --control-file PATH      Live control file to poll for updates\n"
      << "  --http-host HOST         Embedded web server host, default 127.0.0.1\n"
      << "  --http-port PORT         Embedded web server port, default disabled\n"
      << "  --http-only              Serve the UI/API without opening audio I/O\n"
      << "  --alsa-device NAME       Linux ALSA device for in/out, e.g. plughw:2,0\n"
      << "  --alsa-input NAME        Linux ALSA input device\n"
      << "  --alsa-output NAME       Linux ALSA output device\n"
      << "  --drive-db VALUE         Tube stage drive override\n"
      << "  --level-db VALUE         Tube stage level override\n"
      << "  --bright-db VALUE        Tube stage bright override\n"
      << "  --bias-trim VALUE        Tube stage bias override\n"
      << "  --bass VALUE             Tone stack bass 0..10 override\n"
      << "  --mid VALUE              Tone stack mid 0..10 override\n"
      << "  --treble VALUE           Tone stack treble 0..10 override\n"
      << "  --presence VALUE         Tone stack presence 0..10 override\n"
      << "  --power-drive-db VALUE   Master / phase inverter drive override\n"
      << "  --power-level-db VALUE   Final power-stage output trim override\n"
      << "  --power-bias-trim VALUE  Power-stage bias override\n"
      << "  --effect-drive VALUE     Shared effect control 1, default 0.5\n"
      << "  --effect-tone VALUE      Shared effect control 2, default 0.5\n"
      << "  --effect-level-db VALUE  Effect output trim, default 0\n"
      << "  --effect-clean-blend V   Shared effect control 4, default 0.45\n"
      << "  --list-devices           Print PortAudio devices and exit\n";
}

bool ParseDoubleArg(const std::string& arg,
                    const std::string& flag,
                    int& index,
                    int argc,
                    char** argv,
                    double& value_out) {
  const std::string prefix = flag + "=";
  try {
    if (arg == flag) {
      if (index + 1 >= argc) {
        std::cerr << "Missing value for " << flag << "\n";
        return false;
      }
      value_out = std::stod(argv[++index]);
      return true;
    }
    if (arg.rfind(prefix, 0) == 0) {
      value_out = std::stod(arg.substr(prefix.size()));
      return true;
    }
  } catch (const std::exception&) {
    std::cerr << "Invalid value for " << flag << "\n";
    return false;
  }
  return false;
}

bool ParseOptionalDoubleArg(const std::string& arg,
                            const std::string& flag,
                            int& index,
                            int argc,
                            char** argv,
                            std::optional<double>& value_out) {
  double parsed = 0.0;
  if (!ParseDoubleArg(arg, flag, index, argc, argv, parsed)) {
    return false;
  }
  value_out = parsed;
  return true;
}

bool ParseStringArg(const std::string& arg,
                    const std::string& flag,
                    int& index,
                    int argc,
                    char** argv,
                    std::string& value_out) {
  const std::string prefix = flag + "=";
  if (arg == flag) {
    if (index + 1 >= argc) {
      std::cerr << "Missing value for " << flag << "\n";
      return false;
    }
    value_out = argv[++index];
    return true;
  }
  if (arg.rfind(prefix, 0) == 0) {
    value_out = arg.substr(prefix.size());
    return true;
  }
  return false;
}

bool ParseIntArg(const std::string& arg,
                 const std::string& flag,
                 int& index,
                 int argc,
                 char** argv,
                 int& value_out) {
  const std::string prefix = flag + "=";
  try {
    if (arg == flag) {
      if (index + 1 >= argc) {
        std::cerr << "Missing value for " << flag << "\n";
        return false;
      }
      value_out = std::stoi(argv[++index]);
      return true;
    }
    if (arg.rfind(prefix, 0) == 0) {
      value_out = std::stoi(arg.substr(prefix.size()));
      return true;
    }
  } catch (const std::exception&) {
    std::cerr << "Invalid value for " << flag << "\n";
    return false;
  }
  return false;
}

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

void CheckPa(PaError err, const char* what) {
  if (err != paNoError) {
    std::cerr << what << ": " << Pa_GetErrorText(err) << "\n";
    std::exit(1);
  }
}

void PrintDevices() {
  const int count = Pa_GetDeviceCount();
  for (int i = 0; i < count; ++i) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
    if (!info) {
      continue;
    }
    std::cerr << "[" << i << "] " << info->name
              << " in=" << info->maxInputChannels
              << " out=" << info->maxOutputChannels << "\n";
  }
}

PaDeviceIndex FindDuplexDevice(const std::string& needle) {
  const std::string lowered_needle = ToLower(needle);
  const int count = Pa_GetDeviceCount();
  for (int i = 0; i < count; ++i) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
    if (!info) {
      continue;
    }
    const std::string name = ToLower(info->name ? info->name : "");
    if (name.find(lowered_needle) != std::string::npos &&
        info->maxInputChannels >= kInputChannels &&
        info->maxOutputChannels >= kOutputChannels) {
      return i;
    }
  }
  return paNoDevice;
}

PaDeviceIndex FindInputDevice(const std::string& needle) {
  const std::string lowered_needle = ToLower(needle);
  const int count = Pa_GetDeviceCount();
  for (int i = 0; i < count; ++i) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
    if (!info) {
      continue;
    }
    const std::string name = ToLower(info->name ? info->name : "");
    if (name.find(lowered_needle) != std::string::npos &&
        info->maxInputChannels >= kInputChannels) {
      return i;
    }
  }
  return paNoDevice;
}

PaDeviceIndex FindOutputDevice(const std::string& needle) {
  const std::string lowered_needle = ToLower(needle);
  const int count = Pa_GetDeviceCount();
  for (int i = 0; i < count; ++i) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
    if (!info) {
      continue;
    }
    const std::string name = ToLower(info->name ? info->name : "");
    if (name.find(lowered_needle) != std::string::npos &&
        info->maxOutputChannels >= kOutputChannels) {
      return i;
    }
  }
  return paNoDevice;
}

bool IsEffectEnabled(const std::string& effect_name) {
  return effect_name == "klon" ||
         effect_name == "tubescreamer" ||
         effect_name == "rat" ||
         effect_name == "chorus" ||
         effect_name == "compression" ||
         effect_name == "plate";
}

std::vector<std::string> SplitCommaList(const std::string& value) {
  std::vector<std::string> items;
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t comma = value.find(',', start);
    const std::string item = TrimString(
        value.substr(start, comma == std::string::npos ? std::string::npos
                                                       : comma - start));
    if (!item.empty()) {
      items.push_back(item);
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  return items;
}

void EnableEffectName(RuntimeSettings& settings, const std::string& effect_name) {
  if (effect_name == "compression") {
    settings.compression_enabled = true;
  } else if (effect_name == "klon") {
    settings.klon_enabled = true;
  } else if (effect_name == "tubescreamer") {
    settings.tubescreamer_enabled = true;
  } else if (effect_name == "rat") {
    settings.rat_enabled = true;
  } else if (effect_name == "chorus") {
    settings.chorus_enabled = true;
  } else if (effect_name == "plate") {
    settings.plate_enabled = true;
  }
}

bool HasExplicitEffectChain(const LiveControlState& state) {
  return state.effect_compression_enabled.has_value() ||
         state.effect_klon_enabled.has_value() ||
         state.effect_tubescreamer_enabled.has_value() ||
         state.effect_rat_enabled.has_value() ||
         state.effect_chorus_enabled.has_value() ||
         state.effect_plate_enabled.has_value();
}

std::string EffectChainSummary(const RuntimeSettings& settings) {
  std::vector<std::string> enabled;
  if (settings.compression_enabled) enabled.push_back("compression");
  if (settings.klon_enabled) enabled.push_back("klon");
  if (settings.tubescreamer_enabled) enabled.push_back("tubescreamer");
  if (settings.rat_enabled) enabled.push_back("rat");
  if (settings.chorus_enabled) enabled.push_back("chorus");
  if (settings.plate_enabled) enabled.push_back("plate");
  if (enabled.empty()) {
    return "none";
  }

  std::string text;
  for (std::size_t i = 0; i < enabled.size(); ++i) {
    if (i > 0) {
      text += " -> ";
    }
    text += enabled[i];
  }
  return text;
}

std::optional<PreampProfile> ResolvePreampProfile(const Config& config) {
  if (!config.preamp_file.empty()) {
    std::string error;
    auto profile = LoadPreampProfileFromFile(config.preamp_file, &error);
    if (!profile && !error.empty()) {
      std::cerr << error << "\n";
    }
    return profile;
  }

  if (!config.preamp_name.empty()) {
    std::string error;
    auto profile = LoadPreampProfileFromFile(
        "preamps/" + config.preamp_name + ".preamp", &error);
    if (!profile && !error.empty()) {
      std::cerr << error << "\n";
    }
    return profile;
  }

  return std::nullopt;
}

std::optional<PreampProfile> ResolvePreampProfileByName(
    const std::string& preamp_name) {
  std::string error;
  auto profile = LoadPreampProfileFromFile(
      "preamps/" + preamp_name + ".preamp", &error);
  if (!profile && !error.empty()) {
    std::cerr << error << "\n";
  }
  return profile;
}

std::optional<AmpProfile> ResolveAmpProfile(const Config& config) {
  if (!config.amp_file.empty()) {
    std::string error;
    auto profile = LoadAmpProfileFromFile(config.amp_file, &error);
    if (!profile && !error.empty()) {
      std::cerr << error << "\n";
    }
    return profile;
  }

  if (!config.amp_name.empty()) {
    const std::string path = "amps/" + config.amp_name + ".amp";
    std::string error;
    auto profile = LoadAmpProfileFromFile(path, &error);
    if (!profile && !error.empty()) {
      std::cerr << error << "\n";
    }
    return profile;
  }

  return BuiltinPresetProfile(config.preset);
}

std::optional<AmpProfile> ResolveAmpProfileByName(const std::string& amp_name) {
  std::string error;
  auto profile = LoadAmpProfileFromFile("amps/" + amp_name + ".amp", &error);
  if (!profile && !error.empty()) {
    std::cerr << error << "\n";
  }
  return profile;
}

std::shared_ptr<RuntimeSettings> ResolveRuntimeSettings(
    const AmpProfile& base_profile,
    const Config& config,
    const std::optional<LiveControlState>& live_state) {
  auto settings = std::make_shared<RuntimeSettings>();

  AmpProfile profile = base_profile;
  if (live_state && live_state->amp_name) {
    auto live_profile = ResolveAmpProfileByName(*live_state->amp_name);
    if (live_profile) {
      profile = *live_profile;
    }
  }

  PreampProfile preamp = profile.preamp;
  if (const auto explicit_preamp = ResolvePreampProfile(config)) {
    preamp = *explicit_preamp;
  }
  if (live_state && live_state->preamp_name) {
    auto live_preamp = ResolvePreampProfileByName(*live_state->preamp_name);
    if (live_preamp) {
      preamp = *live_preamp;
    }
  }

  settings->amp_name = profile.name;
  settings->preamp_name = preamp.name;
  settings->preamp_profile = preamp;
  settings->stage_controls = preamp.defaults;
  settings->has_power_stage = profile.has_power_stage;
  settings->power_tube_type = profile.power_tube_type;
  settings->power_controls = profile.power_defaults;
  if (config.power_tube_type) {
    settings->has_power_stage = true;
    settings->power_tube_type = *config.power_tube_type;
  }

  if (config.drive_db) settings->stage_controls.drive_db = *config.drive_db;
  if (config.level_db) settings->stage_controls.level_db = *config.level_db;
  if (config.bright_db) settings->stage_controls.bright_db = *config.bright_db;
  if (config.bias_trim) settings->stage_controls.bias_trim = *config.bias_trim;
  if (config.bass) settings->stage_controls.bass = *config.bass;
  if (config.mid) settings->stage_controls.mid = *config.mid;
  if (config.treble) settings->stage_controls.treble = *config.treble;
  if (config.presence) settings->stage_controls.presence = *config.presence;
  if (config.power_drive_db) settings->power_controls.drive_db = *config.power_drive_db;
  if (config.power_level_db) settings->power_controls.level_db = *config.power_level_db;
  if (config.power_bias_trim) settings->power_controls.bias_trim = *config.power_bias_trim;

  if (config.effect != "none" && IsEffectEnabled(config.effect)) {
    EnableEffectName(*settings, config.effect);
  }
  for (const std::string& effect_name : SplitCommaList(config.effects)) {
    EnableEffectName(*settings, effect_name);
  }

  auto apply_legacy_shared_controls = [&](const std::string& effect_name) {
    if (effect_name == "compression") {
      settings->compressor_controls.sustain = config.effect_drive;
      settings->compressor_controls.attack = config.effect_tone;
      settings->compressor_controls.level_db = config.effect_level_db;
      settings->compressor_controls.blend = config.effect_clean_blend;
    } else if (effect_name == "klon") {
      settings->klon_controls.drive = config.effect_drive;
      settings->klon_controls.tone = config.effect_tone;
      settings->klon_controls.level_db = config.effect_level_db;
      settings->klon_controls.clean_blend = config.effect_clean_blend;
    } else if (effect_name == "tubescreamer") {
      settings->tubescreamer_controls.drive = config.effect_drive;
      settings->tubescreamer_controls.tone = config.effect_tone;
      settings->tubescreamer_controls.level_db = config.effect_level_db;
    } else if (effect_name == "rat") {
      settings->rat_controls.distortion = config.effect_drive;
      settings->rat_controls.filter = config.effect_tone;
      settings->rat_controls.level_db = config.effect_level_db;
    } else if (effect_name == "chorus") {
      settings->chorus_controls.depth = config.effect_drive;
      settings->chorus_controls.tone = config.effect_tone;
      settings->chorus_controls.level_db = config.effect_level_db;
      settings->chorus_controls.mix = config.effect_clean_blend;
    } else if (effect_name == "plate") {
      settings->plate_controls.mix = config.effect_drive;
      settings->plate_controls.brightness = config.effect_tone;
      settings->plate_controls.level_db = config.effect_level_db;
      settings->plate_controls.decay = config.effect_clean_blend;
    }
  };

  if (config.effect != "none" && IsEffectEnabled(config.effect)) {
    apply_legacy_shared_controls(config.effect);
  }

  if (live_state) {
    if (!HasExplicitEffectChain(*live_state) && live_state->effect) {
      EnableEffectName(*settings, EffectTypeName(*live_state->effect));
      apply_legacy_shared_controls(EffectTypeName(*live_state->effect));
    }
    if (live_state->effect_compression_enabled) {
      settings->compression_enabled = *live_state->effect_compression_enabled;
    }
    if (live_state->effect_klon_enabled) {
      settings->klon_enabled = *live_state->effect_klon_enabled;
    }
    if (live_state->effect_tubescreamer_enabled) {
      settings->tubescreamer_enabled = *live_state->effect_tubescreamer_enabled;
    }
    if (live_state->effect_rat_enabled) {
      settings->rat_enabled = *live_state->effect_rat_enabled;
    }
    if (live_state->effect_chorus_enabled) {
      settings->chorus_enabled = *live_state->effect_chorus_enabled;
    }
    if (live_state->effect_plate_enabled) {
      settings->plate_enabled = *live_state->effect_plate_enabled;
    }
    if (live_state->power_tube_type) {
      settings->has_power_stage = true;
      settings->power_tube_type = *live_state->power_tube_type;
    }
    if (live_state->drive_db) settings->stage_controls.drive_db = *live_state->drive_db;
    if (live_state->level_db) settings->stage_controls.level_db = *live_state->level_db;
    if (live_state->bright_db) settings->stage_controls.bright_db = *live_state->bright_db;
    if (live_state->bias_trim) settings->stage_controls.bias_trim = *live_state->bias_trim;
    if (live_state->bass) settings->stage_controls.bass = *live_state->bass;
    if (live_state->mid) settings->stage_controls.mid = *live_state->mid;
    if (live_state->treble) settings->stage_controls.treble = *live_state->treble;
    if (live_state->presence) settings->stage_controls.presence = *live_state->presence;
    if (live_state->power_drive_db) settings->power_controls.drive_db = *live_state->power_drive_db;
    if (live_state->power_level_db) settings->power_controls.level_db = *live_state->power_level_db;
    if (live_state->power_bias_trim) settings->power_controls.bias_trim = *live_state->power_bias_trim;
    if (live_state->effect_drive) {
      if (live_state->effect && !HasExplicitEffectChain(*live_state)) {
        if (*live_state->effect == EffectType::kCompressor) {
          settings->compressor_controls.sustain = *live_state->effect_drive;
        } else if (*live_state->effect == EffectType::kKlon) {
          settings->klon_controls.drive = *live_state->effect_drive;
        } else if (*live_state->effect == EffectType::kTubeScreamer) {
          settings->tubescreamer_controls.drive = *live_state->effect_drive;
        } else if (*live_state->effect == EffectType::kRat) {
          settings->rat_controls.distortion = *live_state->effect_drive;
        } else if (*live_state->effect == EffectType::kChorus) {
          settings->chorus_controls.depth = *live_state->effect_drive;
        } else if (*live_state->effect == EffectType::kPlate) {
          settings->plate_controls.mix = *live_state->effect_drive;
        }
      }
    }
    if (live_state->effect_tone) {
      if (live_state->effect && !HasExplicitEffectChain(*live_state)) {
        if (*live_state->effect == EffectType::kCompressor) {
          settings->compressor_controls.attack = *live_state->effect_tone;
        } else if (*live_state->effect == EffectType::kKlon) {
          settings->klon_controls.tone = *live_state->effect_tone;
        } else if (*live_state->effect == EffectType::kTubeScreamer) {
          settings->tubescreamer_controls.tone = *live_state->effect_tone;
        } else if (*live_state->effect == EffectType::kRat) {
          settings->rat_controls.filter = *live_state->effect_tone;
        } else if (*live_state->effect == EffectType::kChorus) {
          settings->chorus_controls.tone = *live_state->effect_tone;
        } else if (*live_state->effect == EffectType::kPlate) {
          settings->plate_controls.brightness = *live_state->effect_tone;
        }
      }
    }
    if (live_state->effect_level_db) {
      if (live_state->effect && !HasExplicitEffectChain(*live_state)) {
        if (*live_state->effect == EffectType::kCompressor) {
          settings->compressor_controls.level_db = *live_state->effect_level_db;
        } else if (*live_state->effect == EffectType::kKlon) {
          settings->klon_controls.level_db = *live_state->effect_level_db;
        } else if (*live_state->effect == EffectType::kTubeScreamer) {
          settings->tubescreamer_controls.level_db = *live_state->effect_level_db;
        } else if (*live_state->effect == EffectType::kRat) {
          settings->rat_controls.level_db = *live_state->effect_level_db;
        } else if (*live_state->effect == EffectType::kChorus) {
          settings->chorus_controls.level_db = *live_state->effect_level_db;
        } else if (*live_state->effect == EffectType::kPlate) {
          settings->plate_controls.level_db = *live_state->effect_level_db;
        }
      }
    }
    if (live_state->effect_clean_blend) {
      if (live_state->effect && !HasExplicitEffectChain(*live_state)) {
        if (*live_state->effect == EffectType::kCompressor) {
          settings->compressor_controls.blend = *live_state->effect_clean_blend;
        } else if (*live_state->effect == EffectType::kKlon) {
          settings->klon_controls.clean_blend = *live_state->effect_clean_blend;
        } else if (*live_state->effect == EffectType::kChorus) {
          settings->chorus_controls.mix = *live_state->effect_clean_blend;
        } else if (*live_state->effect == EffectType::kPlate) {
          settings->plate_controls.decay = *live_state->effect_clean_blend;
        }
      }
    }
    if (live_state->compressor_sustain) settings->compressor_controls.sustain = *live_state->compressor_sustain;
    if (live_state->compressor_attack) settings->compressor_controls.attack = *live_state->compressor_attack;
    if (live_state->compressor_level_db) settings->compressor_controls.level_db = *live_state->compressor_level_db;
    if (live_state->compressor_blend) settings->compressor_controls.blend = *live_state->compressor_blend;
    if (live_state->klon_drive) settings->klon_controls.drive = *live_state->klon_drive;
    if (live_state->klon_tone) settings->klon_controls.tone = *live_state->klon_tone;
    if (live_state->klon_level_db) settings->klon_controls.level_db = *live_state->klon_level_db;
    if (live_state->klon_clean_blend) settings->klon_controls.clean_blend = *live_state->klon_clean_blend;
    if (live_state->tubescreamer_drive) settings->tubescreamer_controls.drive = *live_state->tubescreamer_drive;
    if (live_state->tubescreamer_tone) settings->tubescreamer_controls.tone = *live_state->tubescreamer_tone;
    if (live_state->tubescreamer_level_db) settings->tubescreamer_controls.level_db = *live_state->tubescreamer_level_db;
    if (live_state->rat_distortion) settings->rat_controls.distortion = *live_state->rat_distortion;
    if (live_state->rat_filter) settings->rat_controls.filter = *live_state->rat_filter;
    if (live_state->rat_level_db) settings->rat_controls.level_db = *live_state->rat_level_db;
    if (live_state->chorus_depth) settings->chorus_controls.depth = *live_state->chorus_depth;
    if (live_state->chorus_tone) settings->chorus_controls.tone = *live_state->chorus_tone;
    if (live_state->chorus_level_db) settings->chorus_controls.level_db = *live_state->chorus_level_db;
    if (live_state->chorus_mix) settings->chorus_controls.mix = *live_state->chorus_mix;
    if (live_state->plate_mix) settings->plate_controls.mix = *live_state->plate_mix;
    if (live_state->plate_brightness) settings->plate_controls.brightness = *live_state->plate_brightness;
    if (live_state->plate_level_db) settings->plate_controls.level_db = *live_state->plate_level_db;
    if (live_state->plate_decay) settings->plate_controls.decay = *live_state->plate_decay;
  }

  return settings;
}

LiveControlState BuildLiveControlState(const RuntimeSettings& settings) {
  LiveControlState state;
  state.amp_name = settings.amp_name;
  state.preamp_name = settings.preamp_name;
  state.power_tube_type = settings.power_tube_type;
  const int enabled_count =
      static_cast<int>(settings.compression_enabled) +
      static_cast<int>(settings.klon_enabled) +
      static_cast<int>(settings.tubescreamer_enabled) +
      static_cast<int>(settings.rat_enabled) +
      static_cast<int>(settings.chorus_enabled) +
      static_cast<int>(settings.plate_enabled);
  if (enabled_count == 1) {
    if (settings.compression_enabled) state.effect = EffectType::kCompressor;
    if (settings.klon_enabled) state.effect = EffectType::kKlon;
    if (settings.tubescreamer_enabled) state.effect = EffectType::kTubeScreamer;
    if (settings.rat_enabled) state.effect = EffectType::kRat;
    if (settings.chorus_enabled) state.effect = EffectType::kChorus;
    if (settings.plate_enabled) state.effect = EffectType::kPlate;
  }
  state.effect_compression_enabled = settings.compression_enabled;
  state.effect_klon_enabled = settings.klon_enabled;
  state.effect_tubescreamer_enabled = settings.tubescreamer_enabled;
  state.effect_rat_enabled = settings.rat_enabled;
  state.effect_chorus_enabled = settings.chorus_enabled;
  state.effect_plate_enabled = settings.plate_enabled;
  state.drive_db = settings.stage_controls.drive_db;
  state.level_db = settings.stage_controls.level_db;
  state.bright_db = settings.stage_controls.bright_db;
  state.bias_trim = settings.stage_controls.bias_trim;
  state.bass = settings.stage_controls.bass;
  state.mid = settings.stage_controls.mid;
  state.treble = settings.stage_controls.treble;
  state.presence = settings.stage_controls.presence;
  if (settings.has_power_stage) {
    state.power_drive_db = settings.power_controls.drive_db;
    state.power_level_db = settings.power_controls.level_db;
    state.power_bias_trim = settings.power_controls.bias_trim;
  }
  if (enabled_count == 1 && state.effect) {
    if (*state.effect == EffectType::kCompressor) {
      state.effect_drive = settings.compressor_controls.sustain;
      state.effect_tone = settings.compressor_controls.attack;
      state.effect_level_db = settings.compressor_controls.level_db;
      state.effect_clean_blend = settings.compressor_controls.blend;
    } else if (*state.effect == EffectType::kKlon) {
      state.effect_drive = settings.klon_controls.drive;
      state.effect_tone = settings.klon_controls.tone;
      state.effect_level_db = settings.klon_controls.level_db;
      state.effect_clean_blend = settings.klon_controls.clean_blend;
    } else if (*state.effect == EffectType::kTubeScreamer) {
      state.effect_drive = settings.tubescreamer_controls.drive;
      state.effect_tone = settings.tubescreamer_controls.tone;
      state.effect_level_db = settings.tubescreamer_controls.level_db;
    } else if (*state.effect == EffectType::kRat) {
      state.effect_drive = settings.rat_controls.distortion;
      state.effect_tone = settings.rat_controls.filter;
      state.effect_level_db = settings.rat_controls.level_db;
    } else if (*state.effect == EffectType::kChorus) {
      state.effect_drive = settings.chorus_controls.depth;
      state.effect_tone = settings.chorus_controls.tone;
      state.effect_level_db = settings.chorus_controls.level_db;
      state.effect_clean_blend = settings.chorus_controls.mix;
    } else if (*state.effect == EffectType::kPlate) {
      state.effect_drive = settings.plate_controls.mix;
      state.effect_tone = settings.plate_controls.brightness;
      state.effect_level_db = settings.plate_controls.level_db;
      state.effect_clean_blend = settings.plate_controls.decay;
    }
  }
  state.compressor_sustain = settings.compressor_controls.sustain;
  state.compressor_attack = settings.compressor_controls.attack;
  state.compressor_level_db = settings.compressor_controls.level_db;
  state.compressor_blend = settings.compressor_controls.blend;
  state.klon_drive = settings.klon_controls.drive;
  state.klon_tone = settings.klon_controls.tone;
  state.klon_level_db = settings.klon_controls.level_db;
  state.klon_clean_blend = settings.klon_controls.clean_blend;
  state.tubescreamer_drive = settings.tubescreamer_controls.drive;
  state.tubescreamer_tone = settings.tubescreamer_controls.tone;
  state.tubescreamer_level_db = settings.tubescreamer_controls.level_db;
  state.rat_distortion = settings.rat_controls.distortion;
  state.rat_filter = settings.rat_controls.filter;
  state.rat_level_db = settings.rat_controls.level_db;
  state.chorus_depth = settings.chorus_controls.depth;
  state.chorus_tone = settings.chorus_controls.tone;
  state.chorus_level_db = settings.chorus_controls.level_db;
  state.chorus_mix = settings.chorus_controls.mix;
  state.plate_mix = settings.plate_controls.mix;
  state.plate_brightness = settings.plate_controls.brightness;
  state.plate_level_db = settings.plate_controls.level_db;
  state.plate_decay = settings.plate_controls.decay;
  return state;
}

int AudioCallback(const void* input_buffer,
                  void* output_buffer,
                  unsigned long frames_per_buffer,
                  const PaStreamCallbackTimeInfo*,
                  PaStreamCallbackFlags,
                  void* user_data) {
  auto* state = static_cast<AppState*>(user_data);
  const auto* in = static_cast<const std::int16_t*>(input_buffer);
  auto* out = static_cast<std::int16_t*>(output_buffer);

  const auto settings = std::atomic_load(&state->settings);
  if (settings) {
    if (state->applied_amp_name != settings->amp_name ||
        state->applied_preamp_name != settings->preamp_name) {
      state->preamp.Reset();
      state->power_stage.Reset();
      state->effect.Reset();
      state->tubescreamer.Reset();
      state->rat.Reset();
      state->chorus.Reset();
      state->compressor.Reset();
      state->plate.Reset();
      state->applied_amp_name = settings->amp_name;
      state->applied_preamp_name = settings->preamp_name;
    }
    const std::string effect_signature = EffectChainSummary(*settings);
    if (state->applied_effect_signature != effect_signature) {
      state->effect.Reset();
      state->tubescreamer.Reset();
      state->rat.Reset();
      state->chorus.Reset();
      state->compressor.Reset();
      state->plate.Reset();
      state->applied_effect_signature = effect_signature;
    }
    if (state->applied_power_stage_enabled != settings->has_power_stage ||
        state->applied_power_tube_type != settings->power_tube_type) {
      state->power_stage.Reset();
      state->applied_power_stage_enabled = settings->has_power_stage;
      state->applied_power_tube_type = settings->power_tube_type;
    }

    state->preamp.SetProfile(settings->preamp_profile);
    state->preamp.SetControls(settings->stage_controls);
    if (settings->has_power_stage) {
      state->power_stage.SetTubeType(settings->power_tube_type);
      state->power_stage.SetControls(settings->power_controls);
    }
    state->chorus.SetControls(settings->chorus_controls);
    state->compressor.SetControls(settings->compressor_controls);
    state->effect.SetControls(settings->klon_controls);
    state->tubescreamer.SetControls(settings->tubescreamer_controls);
    state->rat.SetControls(settings->rat_controls);
    state->plate.SetControls(settings->plate_controls);
  }

  for (unsigned long i = 0; i < frames_per_buffer; ++i) {
    float s = 0.0f;
    if (in) {
      s = static_cast<float>(in[i * kInputChannels] / 32768.0f);
    }

    if (settings && settings->compression_enabled) {
      s = state->compressor.Process(s);
    }
    if (settings && settings->klon_enabled) {
      s = state->effect.Process(s);
    }
    if (settings && settings->tubescreamer_enabled) {
      s = state->tubescreamer.Process(s);
    }
    if (settings && settings->rat_enabled) {
      s = state->rat.Process(s);
    }

    s = state->preamp.Process(s);
    if (settings && settings->has_power_stage) {
      s = state->power_stage.Process(s);
    }
    if (settings && settings->chorus_enabled) {
      s = state->chorus.Process(s);
    }
    if (settings && settings->plate_enabled) {
      s = state->plate.Process(s);
    }

    const float clamped = std::clamp(s, -1.0f, 1.0f);
    const auto pcm = static_cast<std::int16_t>(std::lrint(clamped * 32767.0f));
    out[i * 2 + 0] = pcm;
    out[i * 2 + 1] = pcm;
  }

  return g_running ? paContinue : paComplete;
}
}  // namespace

int main(int argc, char** argv) {
  Config config;
  bool list_devices = false;
  const std::filesystem::path repo_root = RepoRootFromArgv0(argv[0]);

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      return 0;
    }
    if (arg == "--list-devices") {
      list_devices = true;
      continue;
    }
    if (arg == "--http-only") {
      config.http_only = true;
      continue;
    }
    if (ParseStringArg(arg, "--amp", i, argc, argv, config.amp_name) ||
        ParseStringArg(arg, "--amp-file", i, argc, argv, config.amp_file) ||
        ParseStringArg(arg, "--preamp", i, argc, argv, config.preamp_name) ||
        ParseStringArg(arg, "--preamp-file", i, argc, argv, config.preamp_file) ||
        ParseStringArg(arg, "--preset", i, argc, argv, config.preset) ||
        ParseStringArg(arg, "--effect", i, argc, argv, config.effect) ||
        ParseStringArg(arg, "--effects", i, argc, argv, config.effects) ||
        ParseStringArg(arg, "--device", i, argc, argv, config.device_name) ||
        ParseStringArg(arg, "--input-device", i, argc, argv, config.input_device_name) ||
        ParseStringArg(arg, "--output-device", i, argc, argv, config.output_device_name) ||
        ParseStringArg(arg, "--control-file", i, argc, argv, config.control_file) ||
        ParseStringArg(arg, "--http-host", i, argc, argv, config.http_host) ||
        ParseStringArg(arg, "--alsa-device", i, argc, argv, config.alsa_input) ||
        ParseStringArg(arg, "--alsa-input", i, argc, argv, config.alsa_input) ||
        ParseStringArg(arg, "--alsa-output", i, argc, argv, config.alsa_output) ||
        ParseIntArg(arg, "--http-port", i, argc, argv, config.http_port) ||
        ParseOptionalDoubleArg(arg, "--drive-db", i, argc, argv, config.drive_db) ||
        ParseOptionalDoubleArg(arg, "--level-db", i, argc, argv, config.level_db) ||
        ParseOptionalDoubleArg(arg, "--bright-db", i, argc, argv, config.bright_db) ||
        ParseOptionalDoubleArg(arg, "--bias-trim", i, argc, argv, config.bias_trim) ||
        ParseOptionalDoubleArg(arg, "--bass", i, argc, argv, config.bass) ||
        ParseOptionalDoubleArg(arg, "--mid", i, argc, argv, config.mid) ||
        ParseOptionalDoubleArg(arg, "--treble", i, argc, argv, config.treble) ||
        ParseOptionalDoubleArg(arg, "--presence", i, argc, argv, config.presence) ||
        ParseOptionalDoubleArg(arg, "--power-drive-db", i, argc, argv, config.power_drive_db) ||
        ParseOptionalDoubleArg(arg, "--power-level-db", i, argc, argv, config.power_level_db) ||
        ParseOptionalDoubleArg(arg, "--power-bias-trim", i, argc, argv, config.power_bias_trim) ||
        ParseDoubleArg(arg, "--effect-drive", i, argc, argv, config.effect_drive) ||
        ParseDoubleArg(arg, "--effect-tone", i, argc, argv, config.effect_tone) ||
        ParseDoubleArg(arg, "--effect-level-db", i, argc, argv, config.effect_level_db) ||
        ParseDoubleArg(arg, "--effect-clean-blend", i, argc, argv, config.effect_clean_blend)) {
      if (arg == "--alsa-device" || arg.rfind("--alsa-device=", 0) == 0) {
        config.alsa_output = config.alsa_input;
      }
      continue;
    }
    if (arg == "--power-tube" || arg.rfind("--power-tube=", 0) == 0) {
      std::string tube_name;
      if (!ParseStringArg(arg, "--power-tube", i, argc, argv, tube_name)) {
        return 1;
      }
      PowerTubeType type;
      if (!ParsePowerTubeType(tube_name, type)) {
        std::cerr << "Unknown power tube: " << tube_name << "\n";
        return 1;
      }
      config.power_tube_type = type;
      continue;
    }

    std::cerr << "Unknown argument: " << arg << "\n";
    PrintUsage(argv[0]);
    return 1;
  }

  if (config.effect != "none" && !IsEffectEnabled(config.effect)) {
    std::cerr << "Unknown effect: " << config.effect << "\n";
    return 1;
  }
  for (const std::string& effect_name : SplitCommaList(config.effects)) {
    if (!IsEffectEnabled(effect_name)) {
      std::cerr << "Unknown effect in --effects: " << effect_name << "\n";
      return 1;
    }
  }

  if (config.http_port < 0 || config.http_port > 65535) {
    std::cerr << "HTTP port must be between 0 and 65535\n";
    return 1;
  }

  if (config.http_port > 0 && config.control_file.empty()) {
    config.control_file = (repo_root / "web" / "live_state.cfg").string();
  }
  if (!config.control_file.empty()) {
    config.control_file = std::filesystem::absolute(config.control_file).string();
  }

  std::signal(SIGINT, OnSignal);
  std::mutex control_file_mutex;

  const auto profile = ResolveAmpProfile(config);
  if (!profile) {
    return 1;
  }

  std::optional<LiveControlState> live_state;
  if (!config.control_file.empty() && std::filesystem::exists(config.control_file)) {
    LiveControlState loaded_state;
    std::string error;
    {
      std::scoped_lock lock(control_file_mutex);
      if (!LoadLiveControlState(config.control_file, loaded_state, &error)) {
        loaded_state = LiveControlState{};
      }
    }
    if (error.empty()) {
      live_state = loaded_state;
#ifdef __linux__
      if (config.alsa_input.empty() && loaded_state.alsa_input) {
        config.alsa_input = *loaded_state.alsa_input;
      }
      if (config.alsa_output.empty() && loaded_state.alsa_output) {
        config.alsa_output = *loaded_state.alsa_output;
      }
      if (config.alsa_input.empty() && !config.alsa_output.empty()) {
        config.alsa_input = config.alsa_output;
      }
      if (config.alsa_output.empty() && !config.alsa_input.empty()) {
        config.alsa_output = config.alsa_input;
      }
#endif
      if (config.input_device_name.empty() && loaded_state.input_device_name) {
        config.input_device_name = *loaded_state.input_device_name;
      }
      if (config.output_device_name.empty() && loaded_state.output_device_name) {
        config.output_device_name = *loaded_state.output_device_name;
      }
      if (config.device_name == "default" &&
          config.input_device_name.empty() &&
          config.output_device_name.empty() &&
          loaded_state.input_device_name &&
          loaded_state.output_device_name &&
          *loaded_state.input_device_name == *loaded_state.output_device_name) {
        config.device_name = *loaded_state.input_device_name;
      }
    } else {
      std::cerr << error << "\n";
    }
  }

  auto settings = ResolveRuntimeSettings(*profile, config, live_state);

  if (!config.control_file.empty() && !std::filesystem::exists(config.control_file)) {
    std::string error;
    {
      std::scoped_lock lock(control_file_mutex);
      SaveLiveControlState(config.control_file, BuildLiveControlState(*settings), &error);
    }
    if (!error.empty()) {
      std::cerr << error << "\n";
    }
  }

  AppState state;
  state.preamp.SetSampleRate(kSampleRateHz);
  state.power_stage.SetSampleRate(kSampleRateHz);
  state.chorus.SetSampleRate(kSampleRateHz);
  state.compressor.SetSampleRate(kSampleRateHz);
  state.effect.SetSampleRate(kSampleRateHz);
  state.tubescreamer.SetSampleRate(kSampleRateHz);
  state.rat.SetSampleRate(kSampleRateHz);
  state.plate.SetSampleRate(kSampleRateHz);
  std::atomic_store(&state.settings, settings);

  CheckPa(Pa_Initialize(), "Pa_Initialize");

  if (list_devices) {
    PrintDevices();
    CheckPa(Pa_Terminate(), "Pa_Terminate");
    return 0;
  }

  PaStream* stream = nullptr;
  std::string selected_device_label = "http-only";

  if (!config.http_only) {
    PaStreamParameters input_params{};
    PaStreamParameters output_params{};
    input_params.channelCount = kInputChannels;
    input_params.sampleFormat = paInt16;
    output_params.channelCount = kOutputChannels;
    output_params.sampleFormat = paInt16;

#ifdef __linux__
    PaAlsaStreamInfo alsa_input_info{};
    PaAlsaStreamInfo alsa_output_info{};
    const bool use_explicit_alsa = !config.alsa_input.empty() || !config.alsa_output.empty();
    if (use_explicit_alsa) {
      if (config.alsa_input.empty()) {
        config.alsa_input = config.alsa_output;
      }
      if (config.alsa_output.empty()) {
        config.alsa_output = config.alsa_input;
      }

      PaAlsa_InitializeStreamInfo(&alsa_input_info);
      PaAlsa_InitializeStreamInfo(&alsa_output_info);
      alsa_input_info.deviceString = config.alsa_input.c_str();
      alsa_output_info.deviceString = config.alsa_output.c_str();

      input_params.device = paUseHostApiSpecificDeviceSpecification;
      input_params.suggestedLatency = 0.01;
      input_params.hostApiSpecificStreamInfo = &alsa_input_info;

      output_params.device = paUseHostApiSpecificDeviceSpecification;
      output_params.suggestedLatency = 0.01;
      output_params.hostApiSpecificStreamInfo = &alsa_output_info;

      selected_device_label =
          "ALSA in=" + config.alsa_input + " out=" + config.alsa_output;
    } else
#endif
    {
      const bool use_split_devices =
          !config.input_device_name.empty() || !config.output_device_name.empty();
      if (use_split_devices) {
        const std::string input_needle =
            config.input_device_name.empty() ? config.device_name : config.input_device_name;
        const std::string output_needle =
            config.output_device_name.empty() ? config.device_name : config.output_device_name;

        const PaDeviceIndex input_device = FindInputDevice(input_needle);
        if (input_device == paNoDevice) {
          std::cerr << "Could not find input device matching \"" << input_needle << "\"\n";
          PrintDevices();
          CheckPa(Pa_Terminate(), "Pa_Terminate");
          return 1;
        }

        const PaDeviceIndex output_device = FindOutputDevice(output_needle);
        if (output_device == paNoDevice) {
          std::cerr << "Could not find output device matching \"" << output_needle << "\"\n";
          PrintDevices();
          CheckPa(Pa_Terminate(), "Pa_Terminate");
          return 1;
        }

        const PaDeviceInfo* input_info = Pa_GetDeviceInfo(input_device);
        const PaDeviceInfo* output_info = Pa_GetDeviceInfo(output_device);
        input_params.device = input_device;
        input_params.suggestedLatency = input_info->defaultLowInputLatency;
        output_params.device = output_device;
        output_params.suggestedLatency = output_info->defaultLowOutputLatency;
        selected_device_label =
            "input=" + std::string(input_info->name) +
            ", output=" + std::string(output_info->name);
      } else {
        const PaDeviceIndex device = FindDuplexDevice(config.device_name);
        if (device == paNoDevice) {
          std::cerr << "Could not find duplex device matching \"" << config.device_name << "\"\n";
          PrintDevices();
          CheckPa(Pa_Terminate(), "Pa_Terminate");
          return 1;
        }

        const PaDeviceInfo* info = Pa_GetDeviceInfo(device);
        input_params.device = device;
        input_params.suggestedLatency = info->defaultLowInputLatency;
        output_params.device = device;
        output_params.suggestedLatency = info->defaultLowOutputLatency;
        selected_device_label = info->name;
      }
    }

    const PaError support =
        Pa_IsFormatSupported(&input_params, &output_params, kSampleRateHz);
    if (support != paFormatIsSupported) {
      std::cerr << "Device \"" << selected_device_label << "\" does not support "
                << kSampleRateHz << " Hz with "
                << kInputChannels << " input / "
                << kOutputChannels << " output int16.\n";
      CheckPa(Pa_Terminate(), "Pa_Terminate");
      return 1;
    }

    CheckPa(Pa_OpenStream(&stream,
                          &input_params,
                          &output_params,
                          kSampleRateHz,
                          kFramesPerBuffer,
                          paNoFlag,
                          AudioCallback,
                          &state),
            "Pa_OpenStream");

#ifdef __linux__
    if (!config.alsa_input.empty() || !config.alsa_output.empty()) {
      PaAlsa_EnableRealtimeScheduling(stream, 1);
    }
#endif

    CheckPa(Pa_StartStream(stream), "Pa_StartStream");
  }

  std::optional<EmbeddedHttpServer> http_server;
  if (config.http_port > 0) {
    http_server.emplace(
        repo_root, config.control_file, config.http_host, config.http_port,
        &control_file_mutex);
    if (!http_server->Start()) {
      if (stream) {
        CheckPa(Pa_StopStream(stream), "Pa_StopStream");
        CheckPa(Pa_CloseStream(stream), "Pa_CloseStream");
      }
      CheckPa(Pa_Terminate(), "Pa_Terminate");
      return 1;
    }
  }

  std::cout << "Running "
            << (config.http_only ? "in HTTP-only mode"
                                 : "on " + selected_device_label +
                                       " at " + std::to_string(kSampleRateHz) + " Hz")
            << ", amp=" << settings->amp_name
            << ", preamp=" << settings->preamp_name
            << (settings->has_power_stage
                    ? std::string(", power=") + PowerTubeTypeName(settings->power_tube_type)
                    : std::string())
            << ", effects=" << EffectChainSummary(*settings)
            << ". Press Ctrl+C to quit.\n";
  if (http_server) {
    std::cout << "Serving UI on http://" << config.http_host << ":"
              << config.http_port << " using control file "
              << config.control_file << "\n";
  }

  while (g_running && (config.http_only || Pa_IsStreamActive(stream) == 1)) {
    if (!config.control_file.empty() && std::filesystem::exists(config.control_file)) {
      static std::filesystem::file_time_type last_write_time;
      std::filesystem::file_time_type current_write_time;
      {
        std::scoped_lock lock(control_file_mutex);
        current_write_time = std::filesystem::last_write_time(config.control_file);
      }
      if (current_write_time != last_write_time) {
        last_write_time = current_write_time;
        LiveControlState loaded_state;
        std::string error;
        bool loaded = false;
        {
          std::scoped_lock lock(control_file_mutex);
          loaded = LoadLiveControlState(config.control_file, loaded_state, &error);
        }
        if (loaded) {
          auto next_settings = ResolveRuntimeSettings(*profile, config, loaded_state);
          std::atomic_store(&state.settings, next_settings);
        } else {
          std::cerr << error << "\n";
        }
      }
    }
    Pa_Sleep(100);
  }

  if (http_server) {
    http_server->Stop();
  }

  if (stream) {
    CheckPa(Pa_StopStream(stream), "Pa_StopStream");
    CheckPa(Pa_CloseStream(stream), "Pa_CloseStream");
  }
  CheckPa(Pa_Terminate(), "Pa_Terminate");
  return 0;
}
