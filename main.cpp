#include <stdio.h>
#include <fstream>
#include <unistd.h>
#include <termios.h>
#include <chrono>
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
  int rowoff = 0;
  int coloff = 0;
  int cx = 0; //cursor x (the top is 0)
  int cy = 0; //cursor y (left is 0)
};
Context context;
struct Node {
  int start;
  int length;
  bool added;
  std::vector<int> newlineOffsets; //offset of \n w.r.t. the beginning of string node points to

  Node(int _start, int _length, bool _added, const std::vector<int>& _newlineOffsets): start(_start), length(_length), added(_added), newlineOffsets(_newlineOffsets) {}
};
struct Edit {
  long time;
  bool isInsert;
  int position = -1; //-1 means edit is empty
  std::string str;
};
struct PieceTable {
  std::string orig;
  std::string added;
  std::vector<Node> nodes;
  Edit tmp; //not applied yet
};
struct FileBuffer {
  PieceTable pt;
  bool dirty = false;
  std::string filename;
  std::string message = "[message]";
  std::string status = "[stauts]";

  FileBuffer(const std::string& _filename): filename(_filename){}
};

FileBuffer* devFb;
std::ofstream log("log");

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
constexpr std::string_view clearLine = "\x1b[0K";
constexpr std::string_view line  = "\x1b[0K\r\n";
constexpr std::string_view gohome = "\x1b[H";
constexpr std::string_view hidecursor = "\x1b[?25l";
}

void findNewLines(const std::string& s, std::vector<int>& newlineOffsets) {
  for (int i = 0; i<(int)s.size(); i++) {
    if (s[i] == '\n') {
      newlineOffsets.push_back(i);
    }
  }
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
}

//returns the last newline offset
int drawNLines(std::string& output, char* buffer, Node& node, int n) {
  int nlOff = -1;
  int prevNlOff = node.start-1;
  for (int y=0; y<n; y++) {
    char* start = &buffer[prevNlOff+1];
    nlOff = node.newlineOffsets[y];
    int size = nlOff - prevNlOff; //\n inclusive size of string
    drawLine(output, start, size-1); //-1 to exclude \n
    output += esq::line;

    prevNlOff = nlOff;
  }

  return nlOff;
}
//fill lines. if node has lines more than enough, return is <=0
int drawNode(std::string& output, int linesToFill, FileBuffer& fb, Node& node) {
  char* buffer = node.added ? fb.pt.added.data() : fb.pt.orig.data();
  int nlines = node.newlineOffsets.size(); //number of lines node can COMPLETELY fill (no room for append)
  if (nlines == 0) {
    //node has no newline
    drawLine(output, &buffer[node.start], node.length);
    return linesToFill;
  }
  
  //yet to be filled *after* this func finishes
  int linesYetFilled = linesToFill - nlines;
  if (linesYetFilled <= 0) {// node has more lines than linesToFill
    drawNLines(output, buffer, node, linesToFill);
  } else {
    int nlOff = drawNLines(output, buffer, node, nlines);
    
    //"asdfasdf\n" length-nloff = 1
    //"asdf\na" length - nloff = 2
    // 0    45                   69
    //"all:\nasdfl;kjasdkfa;jsdf"
    int rest = node.length-nlOff-1;
    if (rest > 0) {
      //if Edit.str has content after the last newline

      drawLine(output, &buffer[nlOff+1], rest);
    }
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
    linesYetFilled = drawNode(output, linesYetFilled, fb, node);
  }

  if (linesYetFilled>0) {
    drawTildes(output, linesYetFilled);
  }

  output.append("\x1b[7m");
  output.append(fb.status);
  output.append(esq::line);
  output.append("\x1b[m");
  output.append(fb.message);
}
void drawCursor(std::string& output) {
  auto row = context.rowoff + context.cy;
  output.append("\x1b[" + std::to_string(context.cy + 1) + ';' + std::to_string(context.cx + 1) + 'H');
  output.append("\x1b[?25h");
}
void drawScreen(FileBuffer& fb) {
  std::string output;
  output += esq::hidecursor;
  output += esq::gohome;
  drawBuffer(output, fb);
  drawCursor(output);
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
    if (seq[1] >= '0' && seq[1] <= '9') {
      if (read(STDIN_FILENO, seq+2, 1) == 0)
        return ESC;
      if (seq[2] == '~') {
        switch (seq[1]) {
        case '3':
          return DEL_KEY;
        case '5':
          return PAGE_UP;
        case '6':
          return PAGE_DOWN;
        }
      }
    } else  {
      switch (seq[1]) {
      case 'A':
        return ARROW_UP;
      case 'B':
        return ARROW_DOWN;
      case 'C':
        return ARROW_RIGHT;
      case 'D':
        return ARROW_LEFT;
      case 'H':
        return HOME_KEY;
      case 'F':
        return END_KEY;
      }
        
    }
  }
  return 0;
}
int getVisualRowSize(const PieceTable& pt, int row) {
  for (int i = 0; i<(int)pt.nodes.size(); i++) {
    const auto& node = pt.nodes[i];
    int nlines = node.newlineOffsets.size();
  }
}
//node index at the beginning of row.
//row is 0 oriented, w.r.t. file
int getRowSize(const PieceTable& pt, int row) {
  int remain = row;
  int acc = 0;
  for (int i = 0; i<(int)pt.nodes.size(); i++) {
    const auto& node = pt.nodes[i];
    int nlines = node.newlineOffsets.size();
    int tmp = remain - nlines;
    if (tmp<0) {
      if (nlines == 1) {
        return node.length + acc;
      } else  {
        return node.newlineOffsets[remain]-node.newlineOffsets[remain-1];
      }
    } else if (tmp == 0) {
      if (nlines == 0) {
        acc+=node.length;
      } else {
        acc = node.length - node.newlineOffsets[nlines-1]-1;
      }
    }
    remain = tmp;
  }

  return 0;
}
//get node that position points to, and offset w.r.t. the buffer of that node type
void getNodeIdxAndBufOffset(int& nodeIdx, int& bufOffset, const PieceTable& pt, int position) {
  int remainingOffset = position;
  for (int i = 0; i<(int)pt.nodes.size(); i++) {
    const auto& node = pt.nodes[i];
    int nodeLen = node.length;
    if (remainingOffset <= nodeLen) {
      //i, node.offset + remainingOffset;
      nodeIdx = i;
      bufOffset = node.start + remainingOffset;
      return;
    }
    remainingOffset -= nodeLen;
  }

  throw std::runtime_error("offset out of bounds");
}
void applyTmpEdit(PieceTable& pt) {
  auto& str = pt.tmp.str;
  auto position = pt.tmp.position;
  
  int addBufSize = pt.added.size();
  pt.added += str;
  int nodeIdx, bufOffset;
  getNodeIdxAndBufOffset(nodeIdx, bufOffset, pt, position);
  auto& node = pt.nodes[nodeIdx];
  if (node.added && bufOffset == node.start+node.length && bufOffset == addBufSize) {
    node.length += str.size();
    return;
  }
  
  
  std::vector<int> newlineOffsets;
  findNewLines(str, newlineOffsets);
  if (!node.added && bufOffset == 0) {
    pt.nodes.insert(pt.nodes.begin(),
                    Node(0, str.size(), true, std::move(newlineOffsets)));
    return;
  }
  
  // exit(1);
  // Node n1{.start=node.start, .length=bufOffset-node.start, .added=node.added};
  // Node n2{.start=addBufSize, .length=(int)s.size(), .added=true};
  
  // pt.nodes.erase(pt.nodes.begin() + nodeIdx);
  // pt.nodes.insert(pt.nodes.begin() + nodeIdx);
}
void insertNewLine(PieceTable& pt) {
  int row = context.rowoff + context.cy;
  int col = context.coloff + context.cx;

  pt.added += '\n';
  // pt.nodes //what to do with node?
}
void mergeEdit(Edit& tmp, Edit& e) {
  if (tmp.position == -1) {
    tmp = std::move(e);
    devFb->message = "merged:" + std::to_string(tmp.position);
    devFb->message += esq::clearLine;
    return;
  }
  // tmp.str += e.str;
}
bool canMergeEdit(Edit& tmp, Edit& e) {
  if (tmp.position == -1) {
    return true;
  }
  if (tmp.isInsert != e.isInsert || tmp.time - e.time > 2000) {
    return false;
  }
  if (tmp.isInsert) {
    return (tmp.position + (int)tmp.str.size()) == e.position;
  } else {
    return tmp.position == e.position;
  }
}
inline long millisec() {
  auto currentTime = std::chrono::system_clock::now();
  return std::chrono::time_point_cast<std::chrono::milliseconds>(currentTime).time_since_epoch().count();
}
void insertChar(PieceTable& pt, char c) {
  int position = 0; //TODO for now add to the beginning of file
  
  int nodeIdx, bufOffset;
  getNodeIdxAndBufOffset(nodeIdx, bufOffset, pt, position);

  Edit e{.time = millisec(),
         .isInsert = true,
         .position = position,
         .str = {c}};

  if (canMergeEdit(pt.tmp, e)) {
    mergeEdit(pt.tmp, e);
  } else {
    applyTmpEdit(pt);
    pt.tmp = std::move(e);
  }
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
void move_cursor(PieceTable& pt, int key) {
  auto filerow = context.rowoff + context.cy;
  auto filecol = context.coloff + context.cx;
  
  switch (key) {
  case ARROW_LEFT:
    if (context.cx == 0) {
      if (context.coloff>0) {
        context.coloff--;
      } else {
        if (filerow > 0) {
          //go to the end of previous line
          context.cy--;
          
          context.cx = getRowSize(pt, filerow-1)-1;
          if (context.cx > context.term_width - 1) {
            context.coloff = context.cx - context.term_width + 1;
            context.cx = context.term_width - 1;
          }
        }
      }
    } else {
      context.cx -= 1;
    }
    break;
  case ARROW_DOWN:
    context.cy++;
    break;
    
  }
  
  // case term::ARROW_RIGHT:
  //   if (row && filecol < row->chars.size()) {
  //     if (E.cx == E.screencols - 1) {
  //       E.coloff++;
  //     } else {
  //       E.cx += 1;
  //     }
  //   } else if (row && filecol == row->chars.size()) {
  //     E.cx = 0;
  //     E.coloff = 0;
  //     if (E.cy == E.screenrows - 1) {
  //       E.rowoff++;
  //     } else {
  //       E.cy += 1;
  //     }
  //   }
  //   break;
  // case term::ARROW_UP:
  //   if (E.cy == 0) {
  //     if (E.rowoff)
  //       E.rowoff--;
  //   } else {
  //     E.cy -= 1;
  //   }
  //   break;
  // case term::ARROW_DOWN:
  //   if (filerow < E.row.size()) {
  //     if (E.cy == E.screenrows - 1) {
  //       E.rowoff++;
  //     } else {
  //       E.cy += 1;
  //     }
  //   }
  //   break;
  // }
  // /* Fix cx if the current line has not enough chars. */
  // filerow = E.rowoff + E.cy;
  // filecol = E.coloff + E.cx;
  // row = (filerow >= E.row.size()) ? nullptr : &E.row[filerow];
  // rowlen = row ? row->chars.size() : 0;
  // if (filecol > rowlen) {
  //   const auto reminder = filecol - rowlen;
  //   if (reminder > E.cx) {
  //     E.coloff -= reminder - E.cx;
  //     E.cx = 0;
  //   } else {
  //     E.cx -= reminder;
  //   }
  // }
}
void processKey(FileBuffer& fb) {
  int key = readKey();
  if (key == CTRL_Q) {
    kill();
  }

  switch (key) {
  case ENTER:
    insertNewLine(fb.pt);
    break;
  case ARROW_DOWN:
  case ARROW_UP:
  case ARROW_LEFT:
  case ARROW_RIGHT:
    move_cursor(fb.pt, key);
    break;
  default:
    insertChar(fb.pt, key);
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

void readLines(FILE* fp, std::string& orig, std::vector<int>& newlineOffsets) {
  char* line = NULL;
  size_t linecap = 0;
  int offset = -1;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    offset+=linelen; //offset of \n
    newlineOffsets.push_back(offset);
    orig.append(line);
  }
  //last line might not contain \n at the end
  if (orig[orig.size()-1] != '\n') {
    newlineOffsets.pop_back();
  }
}
int openFile(const char* filename, PieceTable& pt) {
  FILE* fp = fopen(filename, "r");
  if (!fp) return -1;
  defer [&fp](){fclose(fp);};
  
  fseek(fp, 0, SEEK_END);
  auto filesize = ftell(fp);
  if (filesize > 1073741824) {
    throw std::runtime_error("file too large");
  }
  fseek(fp, 0, SEEK_SET);
  
  
  std::vector<int> newlineOffsets;
  readLines(fp, pt.orig, newlineOffsets);
  pt.nodes.push_back(Node(0, pt.orig.size(), false, std::move(newlineOffsets)));
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
  
  FileBuffer fb{argv[1]};
  devFb = &fb;
  if (openFile(argv[1], fb.pt) == -1) {
    printf("file not exist\r\n");
    // return 0;
  }
  while (context.running) {
    drawScreen(fb);
    processKey(fb);
  }
  return 0;
}
