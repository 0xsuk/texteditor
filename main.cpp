#include <stdio.h>
#include <fstream>
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
  std::string orig;
  std::string added;
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

namespace esq {//Escape SeQuence
constexpr std::string_view line  = "\x1b[0K\r\n";
constexpr std::string_view gohome = "\x1b[H";
constexpr std::string_view hidecursor = "\x1b[?25l";
}

template <size_t N>
void writeN(const char (&s)[N]) {
  write(STDOUT_FILENO, s, N-1);
}
//len is \n exclusive 
void drawLine(std::string& output, const char* from, int len) {
  for (int i = 0; i<len; i++) {
    char c = from[i];
    if (c == TAB) {
      output.append("    ");
      continue;
    }
    output += c;
  }
  output.append(esq::line);
}
//fill lines. if node has lines more than enough, return is <=0
int drawNode(std::string& output, int linesToFill, FileBuffer& fb, Node& node) {
  //yet to be filled *after* this func finishes
  int linesYetFilled = linesToFill - (int)node.lineStarts.size();
  char* orig = fb.pt.orig.data();
  
  if (linesYetFilled < 0) {// node has more lines than linesToFill
    for (int y=0; y<linesToFill; y++) {
      int start = node.lineStarts[y];
      char* from = &orig[start];
      int len = node.lineStarts[y+1]-start-1; //line len, excluding newline
      drawLine(output, from, len);
    }
  } else {
    for (int y=0; y<(int)node.lineStarts.size()-1; y++) {
      int start = node.lineStarts[y];
      char* from = &orig[start];
      int len = node.lineStarts[y+1]-start-1; //line len, excluding newline
      drawLine(output, from, len);
    }
    //for the last line, line might not contain newline.
    int start = node.lineStarts[node.lineStarts.size()-1];
    char* from = &orig[start]; //start of last line
    int len = node.length - start;
    //may or may not contain \n at the end
    if (from[len-1] == '\n') {
      len--;
    }
    drawLine(output, from, len);
  }
  
  return linesYetFilled;
}
//if file is shorter than screen height, write "~" to indicate the row emptyness
void drawTildes(std::string& output, int lines) {
  for (int y = 0; y<lines; y++) {
    output.append("~");
    output.append(esq::line);
  }
}
int STATUS_HEIGHT = 2;
void drawBuffer(std::string& output, FileBuffer& fb) {
  int linesYetFilled = context.term_height-STATUS_HEIGHT;
  for (int i = 0; i<(int)fb.pt.nodes.size() && linesYetFilled>0; i++) {
    auto& node = fb.pt.nodes[i];
    if (node.type == NodeType::Original) {
      linesYetFilled = drawNode(output, linesYetFilled, fb, node);
    }
  }

  if (linesYetFilled>0) {
    drawTildes(output, linesYetFilled);
  }

  output.append("status");
  output.append(esq::line);
  output.append("message");
  // output.append(esq::line);
}
void drawScreen(FileBuffer& fb) {
  std::string output;
  output += esq::hidecursor;
  output += esq::gohome;
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
  if (c == 'q') {
    kill();
  }
}

template <typename T>
class Defer {
  T f;
public:
  Defer(T&& f_) : f(std::forward<T>(f_)) {}
  ~Defer(){ f(); }
};

#define CONCATINATE_(x,y) x ## y
#define CONCATINATE(x,y) CONCATINATE_(x, y)
#define defer Defer CONCATINATE(__defer__,__LINE__)=

int openFile(const char* filename, FileBuffer& fb) {
  FILE* fp = fopen(filename, "r");
  if (!fp) return -1;
  defer [&fp](){fclose(fp);};
  
  fseek(fp, 0, SEEK_END);
  auto filesize = ftell(fp);
  if (filesize > 1073741824) {
    throw std::runtime_error("file too large");
  }
  fseek(fp, 0, SEEK_SET);
  Node n{.type=NodeType::Original, .start=0, .length=0, .lineCount =0};
  char* line = NULL;
  size_t linecap = 0;
  int acc = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    n.lineCount++;
    n.lineStarts.push_back(acc);
    acc+=linelen;
    fb.pt.orig.append(line);
  }
  n.length = acc;

  fb.pt.nodes.push_back(std::move(n));
  return 0;
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
  
  FileBuffer fb;
  if (openFile(argv[1], fb) == -1) {
    printf("file not exist\r\n");
    // return 0;
  }
  while (context.running) {
    drawScreen(fb);
    processKey(fb);
  }
  return 0;
}
