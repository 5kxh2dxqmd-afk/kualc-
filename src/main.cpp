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
static void eips(int row, const std::string& text) {
  // eips is present on supported Kindle firmware and performs the
  // firmware-specific e-ink refresh; calling it avoids hard-coding private
  // framebuffer update ioctls that differ by generation.
  std::string command = "eips 2 " + std::to_string(row) + " " + shell_quote(text) + " >/dev/null 2>&1";
  run_quietly(command);
}
static int screen_rows() {
  int fd = open("/dev/fb0", O_RDONLY); if (fd < 0) return 24; fb_var_screeninfo info {};
  bool ok = ioctl(fd, FBIOGET_VSCREENINFO, &info) == 0; close(fd); return ok ? std::max(12U, info.yres / 24) : 24;
}
enum class Input { Up, Down, Select, Back };
struct InputEvent { Input kind; int x = -1, y = -1, x_max = 0, y_max = 0; };
static InputEvent next_input() {
  std::vector<int> fds, x_max, y_max, last_x, last_y; std::vector<bool> touched;
  auto close_inputs = [&fds] { for (int fd : fds) close(fd); };
  for (int n = 0; n < 16; ++n) { std::string path = "/dev/input/event" + std::to_string(n); int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK); if (fd < 0) continue;
    input_absinfo abs {}; int xmaximum = ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &abs) == 0 ? abs.maximum : 0;
    int ymaximum = ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs) == 0 ? abs.maximum : 0;
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
static std::string clipped(const std::string& text, size_t width = 56) { return text.size() <= width ? text : text.substr(0, width - 3) + "..."; }
static bool run_ui(std::vector<Entry> menu, const std::string& root = "/mnt/us/extensions") {
  constexpr size_t page_size = 10;
  std::vector<std::vector<Entry>*> trail; std::vector<std::string> labels; std::vector<size_t> offsets(1, 0); std::vector<Entry>* current = &menu;
  size_t selected = 0; const int rows = screen_rows(); const int first_row = 5; const int step = std::max(3, (rows - 9) / static_cast<int>(page_size));
  std::string message;
  for (;;) {
    const size_t count = current->size() + 1; size_t& offset = offsets.back(); if (offset >= count) offset = 0;
    if (selected >= count) selected = 0; const size_t last = std::min(count, offset + page_size);
    run_quietly("eips -c >/dev/null 2>&1");
    std::string path = "/"; for (const auto& label : labels) path += label + "/"; eips(1, "#  KUAL  " + clipped(path, 55));
    eips(2, std::string(trail.empty() ? "    " : "[ < Back ]") + "     [ KUAL menu ]     " + (count > page_size ? "[ Next > ]" : ""));
    for (size_t i = offset; i < last; ++i) { const int row = first_row + static_cast<int>(i - offset) * step; std::string label;
      if (i == current->size()) label = "x Quit"; else { Entry& entry = (*current)[i]; label = (entry.checked ? "[x] " : "[ ] ") + entry.name + (!entry.children.empty() ? "  v" : ""); }
      eips(row, std::string(i == selected ? "> [ " : "  [ ") + clipped(label) + " ]"); }
    std::string footer = message.empty() ? "Entries " + std::to_string(offset + 1) + " - " + std::to_string(last) + " of " + std::to_string(count) + "  * KUAL Native" : message;
    eips(rows - 2, clipped(footer, 70)); message.clear();
    InputEvent event = next_input();
    if (event.kind == Input::Back) { if (trail.empty()) return true; current = trail.back(); trail.pop_back(); labels.pop_back(); offsets.pop_back(); selected = 0; continue; }
    if (event.kind == Input::Up) { selected = selected == 0 ? count - 1 : selected - 1; continue; }
    if (event.kind == Input::Down) { selected = (selected + 1) % count; continue; }
    if (event.x >= 0 && event.x_max > 0) { if (event.x * 8 < event.x_max) { if (!trail.empty()) { current = trail.back(); trail.pop_back(); labels.pop_back(); offsets.pop_back(); selected = 0; } continue; }
      if (event.x * 8 > event.x_max * 7) { offset = (offset + page_size < count) ? offset + page_size : 0; selected = offset; continue; }
      if (event.y >= 0 && event.y_max > 0) { int row = event.y * rows / event.y_max; if (row >= first_row) { size_t hit = offset + static_cast<size_t>((row - first_row) / step); if (hit < last) selected = hit; else continue; } else continue; } }
    if (selected == current->size()) return true;
    Entry& entry = (*current)[selected];
    if (!entry.children.empty() && entry.action.empty()) { trail.push_back(current); labels.push_back(entry.name); current = &entry.children; offsets.push_back(0); selected = 0; continue; }
    message = entry.show_action_status ? "Running: " + entry.name : ""; if (!entry.status.empty()) message = entry.status;
    run_quietly("eips -c >/dev/null 2>&1"); eips(rows - 2, clipped(message.empty() ? "Running..." : message, 70));
    int rc = execute(entry); if (entry.check_on_run) entry.checked = true;
    if (entry.refresh) { menu = load_menus(root); trail.clear(); labels.clear(); offsets.assign(1, 0); current = &menu; selected = 0; message = "Menu refreshed."; continue; }
    if (entry.date) { std::time_t now = std::time(nullptr); message = std::ctime(&now); if (!message.empty() && message.back() == '\n') message.pop_back(); }
    else message = rc == 0 ? "Done. Tap to continue" : "Failed (" + std::to_string(rc) + "). Tap to continue";
    if (entry.exit_menu) return true; (void)next_input();
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
