// KUAL Native: a Java-free menu/action engine for jailbroken Kindle devices.
// It intentionally uses only libc/POSIX so it can be cross-compiled for the
// older ARM userlands used by Kindle firmware.
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <ctime>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <variant>
#include <vector>
#ifdef __linux__
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#endif

struct Json {
  using Object = std::map<std::string, Json>;
  using Array = std::vector<Json>;
  std::variant<std::nullptr_t, bool, double, std::string, Array, Object> value;
  bool is_object() const { return std::holds_alternative<Object>(value); }
  bool is_array() const { return std::holds_alternative<Array>(value); }
  const Object& object() const { return std::get<Object>(value); }
  const Array& array() const { return std::get<Array>(value); }
  const std::string* string() const { return std::get_if<std::string>(&value); }
};

class Parser {
 public:
  explicit Parser(const std::string& source) : s_(source) {}
  Json parse() { Json j = value(); ws(); if (p_ != s_.size()) fail("trailing data"); return j; }
 private:
  const std::string& s_; size_t p_ = 0;
  [[noreturn]] void fail(const char* message) const { throw std::runtime_error(std::string(message) + " at byte " + std::to_string(p_)); }
  void ws() { while (p_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[p_]))) ++p_; }
  char take() { if (p_ == s_.size()) fail("unexpected end of JSON"); return s_[p_++]; }
  void expect(char c) { ws(); if (take() != c) fail("unexpected character"); }
  Json value() {
    ws(); if (p_ == s_.size()) fail("expected value");
    switch (s_[p_]) { case '{': return object(); case '[': return array(); case '"': return Json{string_value()};
      case 't': literal("true"); return Json{true}; case 'f': literal("false"); return Json{false};
      case 'n': literal("null"); return Json{nullptr}; default: return number(); }
  }
  void literal(const char* text) { for (const char* c = text; *c; ++c) if (take() != *c) fail("bad literal"); }
  Json object() {
    expect('{'); Json::Object out; ws(); if (p_ < s_.size() && s_[p_] == '}') { ++p_; return Json{out}; }
    for (;;) { ws(); if (p_ == s_.size() || s_[p_] != '"') fail("object key must be a string");
      auto key = string_value(); expect(':'); out.emplace(std::move(key), value()); ws(); char c = take();
      if (c == '}') return Json{out};
      if (c != ',') fail("expected comma");
    }
  }
  Json array() {
    expect('['); Json::Array out; ws(); if (p_ < s_.size() && s_[p_] == ']') { ++p_; return Json{out}; }
    for (;;) { out.push_back(value()); ws(); char c = take(); if (c == ']') return Json{out}; if (c != ',') fail("expected comma"); }
  }
  std::string string_value() {
    expect('"'); std::string out;
    while (true) { char c = take(); if (c == '"') return out; if (static_cast<unsigned char>(c) < 0x20) fail("control character in string");
      if (c != '\\') { out += c; continue; } char e = take();
      switch (e) { case '"': out += '"'; break; case '\\': out += '\\'; break; case '/': out += '/'; break; case 'b': out += '\b'; break; case 'f': out += '\f'; break; case 'n': out += '\n'; break; case 'r': out += '\r'; break; case 't': out += '\t'; break; default: fail("unsupported JSON escape"); }
    }
  }
  Json number() { ws(); char* end = nullptr; const char* begin = s_.c_str() + p_; double n = std::strtod(begin, &end); if (end == begin) fail("expected value"); p_ += static_cast<size_t>(end - begin); return Json{n}; }
};

struct Entry {
  std::string name, action, directory, status;
  int priority = 0;
  bool checked = false, check_on_run = false, refresh = false, show_action_status = true, date = false, hidden = false, exit_menu = true;
  std::vector<Entry> children;
};

static std::optional<std::string> get_string(const Json::Object& o, const char* key) {
  auto it = o.find(key); if (it == o.end()) return std::nullopt; auto v = it->second.string(); return v ? std::optional<std::string>(*v) : std::nullopt;
}
static bool get_bool(const Json::Object& o, const char* key, bool fallback) {
  auto it = o.find(key); if (it == o.end()) return fallback;
  if (auto value = std::get_if<bool>(&it->second.value)) return *value;
  if (auto value = it->second.string()) return *value == "true";
  return fallback;
}
static std::vector<Entry> entries(const Json& node, const std::string& dir) {
  if (!node.is_array()) return {};
  std::vector<Entry> out;
  for (const auto& j : node.array()) { if (!j.is_object()) continue; const auto& o = j.object(); auto name = get_string(o, "name"); if (!name) continue;
    std::string action = get_string(o, "action").value_or("");
    if (auto params = get_string(o, "params")) action += (action.empty() ? "" : " ") + *params;
    int priority = 0; if (auto p = o.find("priority"); p != o.end()) if (auto n = std::get_if<double>(&p->second.value)) priority = static_cast<int>(*n);
    Entry e; e.name = *name; e.action = action; e.directory = dir; e.priority = priority;
    e.check_on_run = get_bool(o, "checked", false); e.refresh = get_bool(o, "refresh", false);
    e.show_action_status = get_bool(o, "status", true); e.date = get_bool(o, "date", false);
    e.hidden = get_bool(o, "hidden", false); e.exit_menu = get_bool(o, "exitmenu", true);
    e.status = get_string(o, "status_text").value_or("");
    auto child = o.find("items"); if (child != o.end()) e.children = entries(child->second, dir);
    if (!e.hidden) out.push_back(std::move(e)); }
  std::stable_sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.priority != b.priority ? a.priority > b.priority : a.name < b.name; });
  return out;
}
static bool directory(const std::string& path) { struct stat st {}; return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
static void load_menus_at(const std::string& root, std::vector<Entry>& result, unsigned depth = 0) {
  // KUAL's original parser searches recursively.  Bound the traversal so a
  // malicious link farm on USB storage cannot exhaust the launcher stack.
  if (depth > 16) return;
  DIR* d = opendir(root.c_str());
  if (!d) return;
  while (dirent* de = readdir(d)) { std::string name(de->d_name); if (name == "." || name == "..") continue;
    std::string path = root + "/" + name; if (!directory(path)) continue;
    std::string menu = path + "/menu.json"; std::ifstream f(menu);
    if (f) { std::stringstream text; text << f.rdbuf(); try { Json doc = Parser(text.str()).parse(); if (doc.is_object()) { auto it = doc.object().find("items"); if (it != doc.object().end()) { auto loaded = entries(it->second, path); result.insert(result.end(), loaded.begin(), loaded.end()); } } }
      catch (const std::exception& e) { std::cerr << "kual-native: ignoring " << menu << ": " << e.what() << "\n"; } }
    load_menus_at(path, result, depth + 1);
  }
  closedir(d);
}
static std::vector<Entry> load_menus(const std::string& root) {
  std::vector<Entry> result; load_menus_at(root, result); return result;
}
[[maybe_unused]] static std::map<std::string, std::string> load_config(const std::string& root) {
  std::map<std::string, std::string> result;
  std::ifstream f(root + "/KUAL.cfg"); std::string line;
  while (std::getline(f, line)) {
    const std::string prefix = "KUAL_"; if (line.compare(0, prefix.size(), prefix) != 0) continue;
    const size_t split = line.find('='); if (split == std::string::npos) continue;
    std::string value = line.substr(split + 1);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
    result[line.substr(prefix.size(), split - prefix.size())] = value;
  }
  return result;
}
static int execute(const Entry& e) {
  if (e.action.empty()) return 0;
  pid_t child = fork();
  if (child < 0) { std::perror("fork"); return 127; }
  if (child == 0) { if (chdir(e.directory.c_str()) != 0) _exit(127); setenv("KUAL_EXTENSION_DIR", e.directory.c_str(), 1); execl("/bin/sh", "sh", "-c", e.action.c_str(), static_cast<char*>(nullptr)); _exit(127); }
  int status = 0; while (waitpid(child, &status, 0) < 0 && errno == EINTR) {} return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}
static void print(const std::vector<Entry>& menu, const std::string& indent = "") { for (const auto& e : menu) { std::cout << indent << e.name; if (!e.action.empty()) std::cout << "\t" << e.action; std::cout << "\n"; print(e.children, indent + "  "); } }
static const Entry* find(const std::vector<Entry>& menu, const std::string& path) {
  const std::vector<Entry>* current = &menu; const Entry* hit = nullptr; std::stringstream parts(path); std::string part;
  while (std::getline(parts, part, '/')) { hit = nullptr; for (const auto& e : *current) if (e.name == part) { hit = &e; break; } if (!hit) return nullptr; current = &hit->children; } return hit;
}

#ifdef __linux__
static std::string shell_quote(const std::string& value) {
  std::string out = "'"; for (char c : value) { if (c == '\'') out += "'\\\"'\\\"'"; else out += c; } return out + "'";
}
static void run_quietly(const std::string& command) {
  const int status = std::system(command.c_str());
  if (status == -1) return;
}
static std::string fbink_path() {
  if (const char* path = std::getenv("FBINK")) return path;
  const char* candidates[] = {"/mnt/us/extensions/KUAL/bin/fbink", "/mnt/us/libkh/bin/fbink", "/mnt/us/koreader/fbink", "/mnt/us/extensions/MRInstaller/bin/KHF/fbink", "fbink"};
  for (const char* candidate : candidates) if (std::string(candidate) == "fbink" || access(candidate, X_OK) == 0) return candidate;
  return "fbink";
}
static bool fbink_available(const std::string& fbink) {
  // FBInk's environment query (-e) crashes in the libkh build shipped on
  // current Kindles.  Asking for help is non-destructive and works across the
  // supported FBInk releases, while still confirming the executable starts.
  return std::system((shell_quote(fbink) + " -q -h >/dev/null 2>&1").c_str()) == 0;
}
struct Screen { int width = 1272, height = 1696; };
static Screen screen_size() {
  int fd = open("/dev/fb0", O_RDONLY); fb_var_screeninfo info {};
  if (fd >= 0 && ioctl(fd, FBIOGET_VSCREENINFO, &info) == 0) { close(fd); return {static_cast<int>(info.xres), static_cast<int>(info.yres)}; }
  if (fd >= 0) close(fd);
  return {};
}
enum class Input { Up, Down, Select, Back };
struct InputEvent { Input kind; int x = -1, y = -1, x_max = 0, y_max = 0; };
static InputEvent next_input() {
  std::vector<int> fds, x_max, y_max, last_x, last_y; std::vector<bool> touched;
  auto close_inputs = [&fds] { for (int fd : fds) close(fd); };
  for (int n = 0; n < 32; ++n) { std::string path = "/dev/input/event" + std::to_string(n); int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK); if (fd < 0) continue;
    input_absinfo abs {}; int xmaximum = ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &abs) == 0 ? abs.maximum : 0;
    int ymaximum = ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs) == 0 ? abs.maximum : 0;
    if (!xmaximum) xmaximum = ioctl(fd, EVIOCGABS(ABS_X), &abs) == 0 ? abs.maximum : 0;
    if (!ymaximum) ymaximum = ioctl(fd, EVIOCGABS(ABS_Y), &abs) == 0 ? abs.maximum : 0;
    fds.push_back(fd); x_max.push_back(xmaximum); y_max.push_back(ymaximum); last_x.push_back(0); last_y.push_back(0); touched.push_back(false); }
  if (fds.empty()) return {Input::Back};
  for (;;) { std::vector<pollfd> watches(fds.size()); for (size_t i = 0; i < fds.size(); ++i) watches[i] = {fds[i], POLLIN, 0}; if (poll(watches.data(), watches.size(), -1) < 0) continue;
    for (size_t i = 0; i < watches.size(); ++i) { if (!(watches[i].revents & POLLIN)) continue; input_event event {};
      while (read(fds[i], &event, sizeof(event)) == sizeof(event)) {
        if (event.type == EV_KEY) {
          if (event.code == BTN_TOUCH) { if (event.value) touched[i] = true; else if (touched[i] && y_max[i] > 0) { close_inputs(); return {Input::Select, last_x[i], last_y[i], x_max[i], y_max[i]}; } }
          else if (event.value == 1) { if (event.code == KEY_UP || event.code == KEY_PAGEUP || event.code == KEY_LEFT) { close_inputs(); return {Input::Up}; }
            if (event.code == KEY_DOWN || event.code == KEY_PAGEDOWN || event.code == KEY_RIGHT) { close_inputs(); return {Input::Down}; }
            if (event.code == KEY_ENTER || event.code == KEY_OK || event.code == KEY_SPACE) { close_inputs(); return {Input::Select}; }
            if (event.code == KEY_BACK || event.code == KEY_HOME || event.code == KEY_ESC) { close_inputs(); return {Input::Back}; } }
        }
        if (event.type == EV_ABS && (event.code == ABS_MT_POSITION_X || event.code == ABS_X)) last_x[i] = event.value;
        if (event.type == EV_ABS && (event.code == ABS_MT_POSITION_Y || event.code == ABS_Y)) last_y[i] = event.value;
      }
    }
  }
}
static std::string clipped(const std::string& text, size_t width) { return text.size() <= width ? text : text.substr(0, width > 3 ? width - 3 : 0) + "..."; }
class Canvas {
 public:
  Canvas(int width, int height) : width_(width), height_(height), pixels_(static_cast<size_t>(width) * height, 255) {}
  void rounded(int x, int y, int width, int height, int radius, unsigned char colour) {
    for (int py = std::max(0, y); py < std::min(height_, y + height); ++py) for (int px = std::max(0, x); px < std::min(width_, x + width); ++px) {
      int dx = 0, dy = 0; if (px < x + radius) dx = x + radius - px; else if (px >= x + width - radius) dx = px - (x + width - radius - 1);
      if (py < y + radius) dy = y + radius - py; else if (py >= y + height - radius) dy = py - (y + height - radius - 1);
      if (dx * dx + dy * dy <= radius * radius) pixels_[static_cast<size_t>(py) * width_ + px] = colour;
    }
  }
  void outline(int x, int y, int width, int height, int radius, unsigned char colour, int stroke = 2) {
    rounded(x, y, width, height, radius, colour); rounded(x + stroke, y + stroke, width - 2 * stroke, height - 2 * stroke, std::max(0, radius - stroke), 255);
  }
  bool save(const std::string& path) const {
    std::ofstream out(path, std::ios::binary); if (!out) return false;
    out << "P5\n" << width_ << " " << height_ << "\n255\n"; out.write(reinterpret_cast<const char*>(pixels_.data()), static_cast<std::streamsize>(pixels_.size())); return static_cast<bool>(out);
  }
 private:
  int width_, height_; std::vector<unsigned char> pixels_;
};
struct Layout { int width, height, top, bottom, rail, gap, button_height, button_gap, content_x, content_width, status_y; };
static Layout make_layout(Screen screen) {
  const auto scale_x = [screen](int value) { return std::max(1, value * screen.width / 1272); };
  const auto scale_y = [screen](int value) { return std::max(1, value * screen.height / 1696); };
  Layout l {screen.width, screen.height, scale_y(62), scale_y(1634), scale_x(136), scale_x(5), 0, scale_y(6), 0, 0, scale_y(1642)};
  l.content_x = l.rail + l.gap * 2; l.content_width = l.width - 2 * l.rail - l.gap * 4;
  l.button_height = (l.bottom - l.top - l.button_gap * 9) / 10; return l;
}
static void print_text(const std::string& fbink, const std::string& text, int px, int top, int bottom, int left, int right, bool centered) {
  const std::string font = "regular=/usr/java/lib/fonts/Futura-Medium.ttf,bold=/usr/java/lib/fonts/Futura-Bold.ttf,px=" + std::to_string(px) + ",top=" + std::to_string(top) + ",bottom=" + std::to_string(bottom) + ",left=" + std::to_string(left) + ",right=" + std::to_string(right);
  std::string command = shell_quote(fbink) + " -q -b -C BLACK -B WHITE " + (centered ? "-m " : "") + "-t " + shell_quote(font) + " " + shell_quote(text) + " >/dev/null 2>&1";
  run_quietly(command);
}
static std::string breadcrumb(const std::vector<std::string>& trail) {
  std::string path = "/"; for (const std::string& label : trail) path += label + "/"; return path;
}
static void draw_ui(const std::string& fbink, const Layout& l, const std::vector<Entry>& current, const std::vector<std::string>& trail, size_t offset, size_t selected, const std::string& message, bool no_status) {
  Canvas canvas(l.width, l.height); const int radius = std::max(8, l.width / 64); const unsigned char border = 110, disabled = 180;
  canvas.outline(l.gap, l.top, l.rail - l.gap, l.bottom - l.top, radius, trail.empty() ? disabled : border);
  canvas.outline(l.width - l.rail + l.gap, l.top, l.rail - l.gap, l.bottom - l.top, radius, border);
  for (size_t row = 0; row < 10; ++row) {
    size_t index = offset + row; if (index > current.size()) break;
    const int y = l.top + static_cast<int>(row) * (l.button_height + l.button_gap);
    canvas.outline(l.content_x, y, l.content_width, l.button_height, radius, index == selected ? 0 : border);
  }
  const std::string image = "/tmp/kual-native-ui.pgm"; if (!canvas.save(image)) return;
  run_quietly(shell_quote(fbink) + " -q -b -g " + shell_quote("file=" + image) + " >/dev/null 2>&1");
  const int header_px = std::max(20, l.height * 34 / 1696), label_px = std::max(22, l.height * 42 / 1696), status_px = std::max(18, l.height * 34 / 1696);
  print_text(fbink, std::string(geteuid() == 0 ? "#  " : "$  ") + "\xE2\x96\xAA  " + clipped(breadcrumb(trail), 58), header_px, 5, l.height - l.top + 5, l.gap, l.width / 2, false);
  if (!trail.empty()) print_text(fbink, "\xE2\x97\x80", label_px, (l.top + l.bottom) / 2 - label_px, l.height - ((l.top + l.bottom) / 2 + label_px), 0, l.width - l.rail, true);
  if (current.size() + 1 > 10) print_text(fbink, "\xE2\x96\xB6", label_px, (l.top + l.bottom) / 2 - label_px, l.height - ((l.top + l.bottom) / 2 + label_px), l.width - l.rail, 0, true);
  const size_t count = current.size() + 1; const size_t last = std::min(count, offset + 10);
  for (size_t index = offset; index < last; ++index) {
    const size_t row = index - offset; const int top = l.top + static_cast<int>(row) * (l.button_height + l.button_gap) + l.button_height / 2 - label_px;
    std::string label = index == current.size() ? (trail.empty() ? "\xC3\x97 Quit" : "/") : current[index].name + (!current[index].children.empty() ? "  \xE2\x96\xBD" : "");
    if (index < current.size() && current[index].checked) label = "\xE2\x9C\x93 " + label;
    print_text(fbink, clipped(label, 54), label_px, top, l.height - (top + label_px * 2), l.content_x + 12, l.width - (l.content_x + l.content_width - 12), true);
  }
  if (!no_status) {
    const std::string footer = message.empty() ? "Entries " + std::to_string(offset + 1) + " - " + std::to_string(last) + " of " + std::to_string(count) + " | KUAL Native | FBInk" : clipped(message, 70);
    print_text(fbink, footer, status_px, l.status_y, 0, l.gap, l.gap, false);
  }
  run_quietly(shell_quote(fbink) + " -q -s -W GC16 >/dev/null 2>&1");
}
static size_t page_offset(size_t offset, int direction, size_t count) {
  constexpr size_t page_size = 10; if (direction > 0) return offset + page_size < count ? offset + page_size : 0;
  if (offset >= page_size) return offset - page_size;
  size_t last = count - count % page_size;
  return last == count && last >= page_size ? last - page_size : last;
}
static bool run_ui(std::vector<Entry> menu, const std::string& root = "/mnt/us/extensions") {
  const std::string fbink = fbink_path(); if (!fbink_available(fbink)) { std::cerr << "kual-native: FBInk is required (set FBINK or install fbink).\n"; return false; }
  const Layout layout = make_layout(screen_size()); const auto config = load_config(root); const bool no_status = config.count("no_show_status") && config.at("no_show_status") == "true";
  size_t page_size = 10; if (auto it = config.find("page_size"); it != config.end()) { try { page_size = std::max<size_t>(1, std::min<size_t>(10, static_cast<size_t>(std::stoul(it->second)))); } catch (...) {} }
  // KUAL's original view has ten fixed grid slots.  Values below ten reserve
  // the unused slots, so external KUAL.cfg files remain safe to use.
  (void)page_size;
  std::vector<std::vector<Entry>*> trail_menus; std::vector<std::string> trail; std::vector<size_t> offsets(1, 0); std::vector<Entry>* current = &menu;
  size_t selected = 0; std::string message;
  for (;;) {
    const size_t count = current->size() + 1; size_t& offset = offsets.back(); if (offset >= count) offset = 0; if (selected >= count) selected = offset;
    draw_ui(fbink, layout, *current, trail, offset, selected, message, no_status); message.clear();
    const InputEvent event = next_input();
    if (event.kind == Input::Back) { if (trail.empty()) return true; current = &menu; trail_menus.clear(); trail.clear(); offsets.assign(1, 0); selected = 0; continue; }
    if (event.kind == Input::Up) { selected = selected == offset ? std::min(count - 1, offset + 9) : selected - 1; continue; }
    if (event.kind == Input::Down) { selected = selected == std::min(count - 1, offset + 9) ? offset : selected + 1; continue; }
    if (event.x >= 0 && event.x_max > 0 && event.y_max > 0) {
      const int x = event.x * layout.width / event.x_max, y = event.y * layout.height / event.y_max;
      if (x < layout.rail) { if (!trail.empty()) { offset = page_offset(offset, -1, count); selected = offset; } continue; }
      if (x >= layout.width - layout.rail) { offset = page_offset(offset, 1, count); selected = offset; continue; }
      if (x < layout.content_x || x >= layout.content_x + layout.content_width || y < layout.top || y >= layout.bottom) continue;
      const size_t row = static_cast<size_t>((y - layout.top) / (layout.button_height + layout.button_gap)); if (row >= 10) continue;
      const size_t hit = offset + row; if (hit >= count) continue; selected = hit;
    }
    if (selected == current->size()) { if (trail.empty()) return true; current = &menu; trail_menus.clear(); trail.clear(); offsets.assign(1, 0); selected = 0; continue; }
    Entry& entry = (*current)[selected];
    if (!entry.children.empty() && entry.action.empty()) { trail_menus.push_back(current); trail.push_back(entry.name); current = &entry.children; offsets.push_back(0); selected = 0; continue; }
    message = entry.show_action_status ? entry.action : ""; draw_ui(fbink, layout, *current, trail, offset, selected, message, no_status);
    const int rc = execute(entry); if (entry.check_on_run) entry.checked = true;
    if (entry.refresh) { menu = load_menus(root); current = &menu; trail_menus.clear(); trail.clear(); offsets.assign(1, 0); selected = 0; message = "Refreshing the menu..."; continue; }
    if (entry.date) { std::time_t now = std::time(nullptr); message = std::ctime(&now); if (!message.empty() && message.back() == '\n') message.pop_back(); }
    else if (!entry.show_action_status) message.clear(); else message = rc == 0 ? entry.action : "Failed (" + std::to_string(rc) + "): " + entry.action;
    if (entry.exit_menu) return true;
  }
}
#endif
int main(int argc, char** argv) {
  std::string root = "/mnt/us/extensions"; bool list = false; std::optional<std::string> run;
  for (int i = 1; i < argc; ++i) { std::string arg = argv[i]; if (arg == "--root" && i + 1 < argc) root = argv[++i]; else if (arg == "--list") list = true; else if (arg == "--run" && i + 1 < argc) run = argv[++i]; else { std::cerr << "Usage: kual-native [--root DIR] --list|--run MENU/PATH\n"; return 2; } }
  auto menu = load_menus(root); if (!list && !run) {
#ifdef __linux__
    return run_ui(menu) ? 0 : 1;
#else
    std::cerr << "The interactive frontend requires Linux/Kindle input and eips.\n"; return 2;
#endif
  }
  if (list) { print(menu); return 0; } const Entry* e = find(menu, *run); if (!e) { std::cerr << "No KUAL entry named: " << *run << "\n"; return 3; } if (!e->children.empty() && e->action.empty()) { print(e->children); return 0; } return execute(*e);
}
