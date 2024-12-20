#include <iostream>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <cstring>
#include <algorithm>
#include <string>
#include <curl/curl.h>
#include <thread> // for sleep_for
#include <chrono> // for chrono literals
#include "./json/json.hpp"
#include "./autoreplyprint.h"

using namespace std;
using json = nlohmann::json;

// config.ini params
std::string http_request="null";
std::string button_com_port="null";
std::string printer_port= "null";
std::string http_error_request = "null";
void* h;
int serial_port;

// ESC POS commands
unsigned char start[] = { 0x1B, 0x3D, 0x01 };            // ESC = 1
unsigned char left_align[] = { 0x1B, 0x61, 0x00 };       // Chapdan yozish
unsigned char right_align[] = { 0x1B, 0x61, 0x02 };      // O'ngdan yozish
unsigned char center_align[] = { 0x1B, 0x61, 0x01 };     // 0x01 = markazlashtirish  port send("0x1B\0x61\0x01salom")
unsigned char bold_on[] = { 0x1B, 0x45, 0x01 };          // 0x01 = qalinlashtirishni yoqish
unsigned char underline_on[] = { 0x1B, 0x2D, 0x01 };     // 0x01 = pastki chiziqni yoqish
unsigned char normal_font[] = { 0x1B, 0x21, 0x00 };      // Normal font (standard size)
unsigned char large_font[] = { 0x1B, 0x21, 0x11 };       // 2x2 font size (large)
unsigned char very_large_font[] = { 0x1B, 0x21, 0x20 };  // 3x3 font size (extra large)
unsigned char newLine[] = { 0x0A };                      // 0x0A (LF) yangi qatorga o'tish

void readConfigFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: " << filename << " fayli topilmadi yoki ochishda xatolik yuz berdi." << std::endl;
        return;
    }
    std::string line;
    auto trim = [](std::string &str) {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            str.clear();
            return;
        }
        size_t last = str.find_last_not_of(" \t\r\n");
        str = str.substr(first, (last - first + 1));
    };
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        size_t pos = line.find("=");
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);
            trim(key);
            trim(value);
            if (key == "http_request") {
                http_request = value;
            }
            else if (key == "button_com_port") {
                button_com_port = value;
            }
            else if (key == "printer_port") {
                printer_port = value;
            }
            else if (key == "http_error_request") {
                http_error_request = value;
            }
        }
    }
    file.close();
}

size_t WriteCallback(void *contents, size_t size, size_t nmemb, string *output) {
    size_t total_size = size * nmemb;
    output->append((char*)contents, total_size);
    return total_size;
}

void sendErrorRequest(const std::string& data) {
    CURL* curl = curl_easy_init();
    if (curl) {
        std::string responseData;
        curl_easy_setopt(curl, CURLOPT_URL, http_error_request.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseData);
        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }
    return;
}


void printFunction(const std::string& responseData) {
    try {
        auto jsonArray = json::parse(responseData);
        if (jsonArray.is_array()) {
            for (const auto& element : jsonArray) {
                if ((!element.contains("type") || !element.contains("align") || !element.contains("font") || !element.contains("body")) ||
                    (element["type"] != "text" && element["type"] != "qrCode") ||
                    (element["align"] != "center" && element["align"] != "left" && element["align"] != "right") ||
                    (element["font"] != "normal" && element["font"] != "bold" && element["font"] != "large" && element["font"] != "underline")) {
                    sendErrorRequest("{\"error\": \"JSON elements are invalid\"}");
                    return;
                }
            }
             // send this cmd to query printer status
             unsigned char cmd[] = { 0x10, 0x04, 0x01 };
             CP_Port_SkipAvailable(h);
             if (CP_Port_Write(h, cmd, sizeof(cmd), 1000) == (int)sizeof(cmd)) {
                unsigned char status;
                if (CP_Port_Read(h, &status, 1, 2000) == 1) {
                   cout.flush();
                   if((int)status==18){
                       CP_Port_Write(h, start, sizeof(start), 1000);
                       for (const auto& element : jsonArray) {
                           //string printString = "";
                           if (element["type"] == "text") {
                              // Find align
                              if (element["align"] == "center") { CP_Port_Write(h, center_align, sizeof(center_align), 1000); }
                              else if (element["align"] == "right") { CP_Port_Write(h, right_align, sizeof(right_align), 1000); }
                              else { CP_Port_Write(h, left_align, sizeof(left_align), 1000); }

                              // Find font
                              if (element["font"] == "bold") { CP_Port_Write(h, bold_on, sizeof(bold_on), 1000); }
                              else if (element["font"] == "large") { CP_Port_Write(h, very_large_font, sizeof(very_large_font), 1000); }
                              else if (element["font"] == "underline") { CP_Port_Write(h, underline_on, sizeof(underline_on), 1000); }
                              else { CP_Port_Write(h, normal_font, sizeof(normal_font), 1000); }

                              // Print body
                              std::string body = element["body"];
                              size_t size = body.size() + 1;  // Null terminator uchun +1
                              wchar_t* buffer = new wchar_t[size];
                              size_t convertedSize = mbstowcs(buffer, body.c_str(), size);
                              if (convertedSize == (size_t)-1) {
                                 std::cerr << "Error converting multibyte string to wide characters" << std::endl;
                                 delete[] buffer;
                                 return;
                              }
                              CP_Pos_PrintTextInUTF8(h, buffer);
                              delete[] buffer;
                          }
                          else if (element["type"] == "qrCode") {
                               // Find align
                               if (element["align"] == "right") {
                                  CP_Port_Write(h, right_align, sizeof(right_align), 1000);
                               }
                               else if (element["align"] == "center") {
                                    CP_Port_Write(h, center_align, sizeof(center_align), 1000);
                               }
                               else {
                                    CP_Port_Write(h, left_align, sizeof(left_align), 1000);
                               }

                               //Print QR body
                               int nQRCodeUnitWidth = 9;  // QR size
                               const std::string& body = element["body"].get<std::string>();
                               char* strQRCode = new char[body.size() + 1];
                               strcpy(strQRCode, body.c_str());
                               CP_Pos_PrintQRCodeUseEpsonCmd(h, nQRCodeUnitWidth, CP_QRCodeECC_M, strQRCode);
                               delete[] strQRCode;
                       }
                       CP_Port_Write(h, newLine, sizeof(newLine), 1000);
                   }
                   CP_Pos_PrintTextInUTF8(h, L"\r\n");
                   CP_Pos_PrintTextInUTF8(h, L"\r\n");
                   CP_Pos_PrintTextInUTF8(h, L"\r\n");
                   CP_Pos_PrintTextInUTF8(h, L"\r\n");
                   CP_Pos_FullCutPaper(h);
                   }else if((int)status==26){
                       sendErrorRequest("{\"error\": \"Printer Paper end\"}");
                       return;
                   }
                   else{
                       sendErrorRequest("{\"error\": \"Printer another error\"}");
                       return;
                   }

                } else {
                   sendErrorRequest("{\"error\": \"Printer Power off\"}");
                   h = CP_Port_OpenUsb(printer_port.c_str(), 0);
                   return;
                }
             } else {
                  sendErrorRequest("{\"error\": \"Printer Power off\"}");
                  h = CP_Port_OpenUsb(printer_port.c_str(), 0);
                  return;
             }

        }
        else {
            sendErrorRequest("{\"error\": \"Array elements are invalid\"}");
            return;
        }
    }
    catch (const nlohmann::json::parse_error& e) {
        std::string errorMessage = "{\"error\": \"Invalid JSON format\"}";
        sendErrorRequest(errorMessage);
    }
}


void listenArduino(const std::string& portname) {
    serial_port = open(portname.c_str(), O_RDONLY | O_NOCTTY);
    if (serial_port == -1) {
        std::cout << "Error opening Arduino" << std::endl;
        sendErrorRequest("{\"error\": \"Error opening Arduino\"}");
        return;
    } else {
        std::cout << "Arduino opened successfully!" << std::endl;
    }

    struct termios tty;
    if (tcgetattr(serial_port, &tty) != 0) {
        std::cerr << "Error getting term attributes: " << strerror(errno) << std::endl;
        return;
    }

    // Portni sozlash: baudrate, parity, bit uzunligi va to'xtash bitlari
    cfsetispeed(&tty, B9600);  // 9600 baud
    cfsetospeed(&tty, B9600);  // 9600 baud
    tty.c_cflag &= ~PARENB;    // no parity
    tty.c_cflag &= ~CSTOPB;    // 1 stop bit
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;        // 8 data bits
    tty.c_cflag &= ~CRTSCTS;   // no hardware flow control
    tty.c_cflag |= CREAD | CLOCAL;  // enable receiver, ignore modem control lines
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);  // disable software flow control
    tty.c_iflag &= ~IGNBRK;  // disable break processing
    tty.c_oflag &= ~OPOST;   // disable output processing
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);  // disable canonical mode, echo, and signals
    tty.c_lflag |= IEXTEN;  // enable extended functions (not needed for Arduino, but just in case)
    if (tcsetattr(serial_port, TCSANOW, &tty) != 0) {
        std::cerr << "Error setting term attributes: " << strerror(errno) << std::endl;
        return;
    }

    char read_buf[256];  // Buffer to hold incoming data
    while (true) {
        // Buferni tozalash
        memset(read_buf, 0, sizeof(read_buf));
        ssize_t n = read(serial_port, read_buf, sizeof(read_buf));
        if (n < 0) {
            std::cerr << "Error reading from serial port" << std::endl;
            tcflush(serial_port, TCIOFLUSH);
            return;
        }else if (n == 0) {
                 close(serial_port);
    tcflush(serial_port, TCIOFLUSH);
                serial_port = open(portname.c_str(), O_RDONLY | O_NOCTTY);
    if (serial_port == -1) {
        std::cout << "Error opening Arduino" << std::endl;
        sendErrorRequest("{\"error\": \"Error opening Arduino\"}");
        return;
    }
        }
        // Har bir o'qilgan baytni tekshirish
        for (ssize_t i = 0; i < n; i++) {
            if (read_buf[i] == '1' && i + 1 < n && read_buf[i + 1] == '2') {
                std::cout << "Detected '12'. Sending HTTP GET..." << std::endl;

                // CURL so'rovini yuborish
                CURL* curl = curl_easy_init();
                if (curl) {
                    std::string responseData;
                    curl_easy_setopt(curl, CURLOPT_URL, http_request.c_str());
                    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
                    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseData);
                    CURLcode res = curl_easy_perform(curl);

                    if (res != CURLE_OK) {
                        std::cerr << "CURL GET error: " << curl_easy_strerror(res) << std::endl;
                        sendErrorRequest("{\"error\": \"Invalid GET command from Arduino\"}");
                    } else {
                        printFunction(responseData);
                    }
                    curl_easy_cleanup(curl);
                }

                // '12' topilganidan so'ng, i ni 2 ga oshirish
                i++;
            }
        }

        // Kichik kutish intervali qo'shish (CPU yukini kamaytirish uchun)
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }

    close(serial_port);
}


int main() {
    readConfigFile("config.ini");

    //cout<<  http_request << endl << button_com_port << endl << printer_port  << endl <<  http_error_request << endl;

    h = CP_Port_OpenUsb(printer_port.c_str(), 0);
    if (h != nullptr) {
        std::cout << "Printer opened sucs!" << std::endl;
    }
    else {
        std::cerr << "Error open printer" << std::endl;
        sendErrorRequest("{\"error\": \"Error open printer\"}");
        std::this_thread::sleep_for(std::chrono::seconds(3));
        main();
    }

    listenArduino(button_com_port);

    CP_Port_Close(h);
    close(serial_port);
    tcflush(serial_port, TCIOFLUSH);
    std::this_thread::sleep_for(std::chrono::seconds(3));
    main();
}