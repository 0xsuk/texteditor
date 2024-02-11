#include <stdio.h>
#include <unistd.h>
#include <termios.h>
#include <stdexcept>
#include <sys/mman.h>
#include <vector>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <functional>
#include "main.h"

struct Context {
  bool running = true;
  int term_width = 0;
  int term_height = 0;
};
Context context;

enum class NodeType {
  Original,
  Added
};
struct Node {
  NodeType type;
  int start;
  int length;
  std::vector<int> lineStarts;
  int lineCount;
};
struct PieceTable {
  char* orig;
  char* added;
  std::vector<Node> nodes;
};

struct FileBuffer {
  PieceTable pt;
  bool dirty = false;
};

enum KEY_ACTION {
  KEY_NULL = 0,    /* NULL */
  CTRL_C = 3,      /* Ctrl-c */
  CTRL_D = 4,      /* Ctrl-d */
  CTRL_F = 6,      /* Ctrl-f */
  CTRL_H = 8,      /* Ctrl-h */
  TAB = 9,         /* Tab */
  CTRL_L = 12,     /* Ctrl+l */
  ENTER = 13,      /* Enter */
  CTRL_Q = 17,     /* Ctrl-q */
  CTRL_S = 19,     /* Ctrl-s */
  CTRL_U = 21,     /* Ctrl-u */
  ESC = 27,        /* Escape */
  BACKSPACE = 127, /* Backspace */
  /* The following are just soft codes, not really reported by the
   * terminal directly. */
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

template <size_t N>
void writeN(const char (&s)[N]) {
  write(STDOUT_FILENO, s, N-1);
}
int STATUS_HEIGHT = 2;
void drawBuffer(std::string& output, FileBuffer& fb) {
  for (int y = 0; y<context.term_height - STATUS_HEIGHT; y++) {
    output += "fuck\r\n";
  }
}
void drawScreen(FileBuffer& fb) {
  std::string output;
  output += "\x1b[?25l"; /* Hide cursor. */
  output += "\x1b[H";    /* Go home. */
  
  drawBuffer(output, fb);
  write(STDOUT_FILENO, output.data(), output.size());
}
int handleEsc() {
  char seq[3];
  int nread = read(STDIN_FILENO, &seq[0], 1);
  if (nread == 0) {//no [ followed. 
    return ESC;
  }
  nread = read(STDIN_FILENO, &seq[1], 1);
  if (nread == 0) {//no seq followed.
    return ESC;
  }

  if (seq[0] == '[') {
    //TODO
  }
  return 0;
}
int readKey() {
  char c;
  int nread = read(STDIN_FILENO, &c, 1);
  if (nread == 0) {
    return 0;
  }
  if (c == ESC) {
    return handleEsc();
  }

  return c;
}
void processKey(FileBuffer& fb) {
  int c = readKey();
  printf("%d %c\r\n", c, c);
  if (c == 'q') {
    kill();
  }
}

template <typename T>
class Defer {
    const T f;
public:
    Defer(const T &f_) : f(f_){}
    ~Defer(){ f(); }
};

#define CONCATINATE(x,y) x ## y
#define defer Defer CONCATINATE(__defer__,__LINE__)=

std::pair<char*, size_t> openFile(const char* filename) {
  int fd = open(filename, O_RDONLY);
  defer [fd]() {close(fd);};
  struct stat sb;
  if (fstat(fd, &sb) == -1) {
    return {nullptr, 0};
  }
  if (sb.st_size > 1073741824) { //1GB
    //TODO: WARNING
    return {nullptr, 0};
  }

  return {(char*)mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0), sb.st_size};
}
int getTermSize(int& width, int& height) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    return -1;
  }

  width = ws.ws_col;
  height = ws.ws_row;
  return 0;
}

struct termios enterRawMode() {
  struct termios orig;
  if (!isatty(STDIN_FILENO)) {
    throw std::runtime_error("stdin isn't a TTY");
  }
  if (tcgetattr(STDIN_FILENO, &orig) == -1) {
    throw std::runtime_error("Unable to get the current terminal state");
  }
  struct termios raw;
  raw = orig;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;
  
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) {
    throw std::runtime_error("Unable to set the terminal to raw mode");
  }
  return orig;
}
void kill() {
  context.running = false;
}
void init() {
  if (getTermSize(context.term_width, context.term_height) == -1) {
    throw std::runtime_error("Failed to get terminal size");
  }
  printf("terminal :%d %d\r\n", context.term_width, context.term_width);
}
int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: ./texteditor filename\n");
    return 0;
  }
  
  init();
  auto orig = enterRawMode();
  defer [&orig]() {
    writeN("\x1b[0;0H\x1b[2J");
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
  };
  
  auto [addr, stsize] = openFile(argv[1]);
  if (addr == nullptr) {
    printf("file not exist\r\n");
    // return 0;
  }
  Node node{.type=NodeType::Original, .start=0, .length=(int)stsize};
  FileBuffer fb;
  fb.pt.orig = addr;
  fb.pt.nodes.push_back(std::move(node));

  while (context.running) {
    drawScreen(fb);
    processKey(fb);
  }
  return 0;
}
