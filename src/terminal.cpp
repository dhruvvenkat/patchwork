#include "terminal.h"

#include <cerrno>
#include <cstring>
#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace patchwork {

namespace {

KeyPress CharacterKey(char ch, bool alt = false) {
    return {.type = KeyType::Character, .ch = ch, .alt = alt};
}

}  // namespace

Terminal::Terminal() : original_mode_(new termios{}) {}

Terminal::~Terminal() {
    if (raw_mode_enabled_) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, original_mode_);
    }
    delete original_mode_;
}

bool Terminal::EnableRawMode(std::string* error) {
    if (tcgetattr(STDIN_FILENO, original_mode_) == -1) {
        if (error != nullptr) {
            *error = std::strerror(errno);
        }
        return false;
    }

    termios raw = *original_mode_;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        if (error != nullptr) {
            *error = std::strerror(errno);
        }
        return false;
    }

    raw_mode_enabled_ = true;
    return true;
}

KeyPress Terminal::ReadKey() const {
    char ch = '\0';
    while (true) {
        const ssize_t bytes_read = read(STDIN_FILENO, &ch, 1);
        if (bytes_read == 1) {
            break;
        }
        if (bytes_read == -1 && errno != EAGAIN) {
            return {};
        }
    }

    if (ch == '\x1b') {
        char seq[3] = {'\0', '\0', '\0'};
        if (read(STDIN_FILENO, &seq[0], 1) != 1) {
            return {.type = KeyType::Escape};
        }
        if (seq[0] == '[') {
            if (read(STDIN_FILENO, &seq[1], 1) != 1) {
                return {.type = KeyType::Escape};
            }
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) {
                    return {.type = KeyType::Escape};
                }
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1':
                        case '7':
                            return {.type = KeyType::Home};
                        case '3':
                            return {.type = KeyType::DeleteKey};
                        case '4':
                        case '8':
                            return {.type = KeyType::End};
                        case '5':
                            return {.type = KeyType::PageUp};
                        case '6':
                            return {.type = KeyType::PageDown};
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A':
                        return {.type = KeyType::ArrowUp};
                    case 'B':
                        return {.type = KeyType::ArrowDown};
                    case 'C':
                        return {.type = KeyType::ArrowRight};
                    case 'D':
                        return {.type = KeyType::ArrowLeft};
                    case 'H':
                        return {.type = KeyType::Home};
                    case 'F':
                        return {.type = KeyType::End};
                }
            }
        } else if (seq[0] == 'O') {
            if (read(STDIN_FILENO, &seq[1], 1) != 1) {
                return {.type = KeyType::Escape};
            }
            switch (seq[1]) {
                case 'H':
                    return {.type = KeyType::Home};
                case 'F':
                    return {.type = KeyType::End};
            }
        } else {
            return CharacterKey(seq[0], true);
        }
        return {.type = KeyType::Escape};
    }

    if (ch == '\r') {
        return {.type = KeyType::Enter};
    }
    if (ch == 127) {
        return {.type = KeyType::Backspace};
    }
    if (ch >= 1 && ch <= 26) {
        return {.type = KeyType::Character, .ch = static_cast<char>('a' + ch - 1), .ctrl = true};
    }
    return CharacterKey(ch);
}

std::pair<int, int> Terminal::WindowSize() const {
    winsize size{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == -1 || size.ws_col == 0) {
        return {24, 80};
    }
    return {size.ws_row, size.ws_col};
}

void Terminal::Write(const std::string& text) const {
    ::write(STDOUT_FILENO, text.c_str(), text.size());
}

}  // namespace patchwork

