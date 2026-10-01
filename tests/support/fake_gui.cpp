// Stands in for puls-gui: writes its arguments, one per line, to the file
// named by PULS_FAKE_GUI_OUTPUT. The file appears complete or not at all.

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    const char* output = std::getenv("PULS_FAKE_GUI_OUTPUT");
    if (output == nullptr) {
        return 3;
    }
    const std::string temporary = std::string(output) + ".tmp";
    std::FILE* file = std::fopen(temporary.c_str(), "wb");
    if (file == nullptr) {
        return 4;
    }
    for (int index = 1; index < argc; ++index) {
        std::fputs(argv[index], file);
        std::fputc('\n', file);
    }
    if (std::fclose(file) != 0 || std::rename(temporary.c_str(), output) != 0) {
        return 5;
    }
    return 0;
}
