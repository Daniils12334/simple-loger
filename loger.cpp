#include <opencv2/opencv.hpp>
#include <tesseract/baseapi.h>
#include <leptonica/allheaders.h>
#include <curl/curl.h>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xinerama.h>
#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <ctime>
#include <map>

#define DEBUG

std::unordered_set<std::string> recentTimes;

// Function to send a message to Discord with retries
bool sendToDiscordWithRetry(const std::string& message, int maxRetries = 3) {
    CURL* curl;
    CURLcode res;
    bool success = false;

    for (int retry = 0; retry < maxRetries; retry++) {
        curl = curl_easy_init();
        if (curl) {
            const std::string webhook_url = "https://discord.com/api/webhooks/1334124265104085044/i4F99g9T1-5rydCr7qbgJ5WHLCPkdpR-098MCv4eFU9BJzwCTvrp7IeIe96_ICTepj5K";

            // Escape newlines and quotes to keep valid JSON format
            std::string json_payload = R"({"content": ")" + message + R"("})";
            json_payload.erase(std::remove(json_payload.begin(), json_payload.end(), '\n'), json_payload.end()); // Remove newlines

            struct curl_slist* headers = nullptr;
            headers = curl_slist_append(headers, "Content-Type: application/json");

            curl_easy_setopt(curl, CURLOPT_URL, webhook_url.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

            res = curl_easy_perform(curl);
            if (res == CURLE_OK) {
                success = true;
                break; // Success, exit retry loop
            } else {
                std::cerr << "curl_easy_perform() failed (retry " << retry + 1 << "): " << curl_easy_strerror(res) << std::endl;
            }

            curl_easy_cleanup(curl);
            curl_slist_free_all(headers);
        }

        // Wait for a short time before retrying
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return success;
}

std::string normalizeText(const std::string& text) {
    std::string cleaned = text;
    // Remove leading/trailing spaces
    cleaned.erase(0, cleaned.find_first_not_of(" \t\n\r"));
    cleaned.erase(cleaned.find_last_not_of(" \t\n\r") + 1);
    // Convert to lowercase
    std::transform(cleaned.begin(), cleaned.end(), cleaned.begin(), ::tolower);
    // Remove all non-alphanumeric characters except spaces
    cleaned.erase(std::remove_if(cleaned.begin(), cleaned.end(),
                  [](unsigned char c) { return !std::isalnum(c) && c != ' '; }),
                  cleaned.end());
    // Remove extra spaces
    cleaned.erase(std::unique(cleaned.begin(), cleaned.end(),
                  [](char a, char b) { return a == ' ' && b == ' '; }),
                  cleaned.end());
    return cleaned;
}

// Function to split log into three parts: day, time, and main log
std::tuple<std::string, std::string, std::string> splitLog(const std::string& log) {
    size_t dayPos = log.find("day");
    if (dayPos == std::string::npos) {
        return {"", "", log}; // No day or time found
    }

    // Extract the day part (e.g., "day XXXX")
    size_t dayEnd = log.find(' ', dayPos + 4); // Skip "day "
    std::string day = log.substr(dayPos, dayEnd - dayPos);

    // Extract the time part (e.g., "HHMMSS")
    size_t timeStart = dayEnd + 1; // Skip the space after the day
    size_t timeEnd = timeStart + 6; // Time is 6 digits (HHMMSS)
    std::string time = log.substr(timeStart, 6);

    // Extract the main log content
    std::string mainLog = log.substr(timeEnd + 1); // Skip the space after the time

    return {day, time, mainLog};
}

// Function to check if the message is new based on time
bool isNewMessage(const std::string& message) {
    auto [day, time, mainLog] = splitLog(message);
    if (time.empty()) {
        return false; // Ignore messages without a valid time
    }

    if (recentTimes.count(time)) return false;

    recentTimes.insert(time);
    return true;
}

int getRightMonitorXOffset() {
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        std::cerr << "Cannot open X display." << std::endl;
        return 0;
    }

    int screen_offset = 0; // Default offset (single monitor scenario)

    if (XineramaIsActive(display)) {
        int numScreens;
        XineramaScreenInfo* screens = XineramaQueryScreens(display, &numScreens);

        if (numScreens > 1) {
            screen_offset = screens[0].width; // X coordinate of right monitor
            std::cout << "Detected multi-monitor setup. Right screen starts at X: " << screen_offset << std::endl;
        }

        XFree(screens);
    }

    XCloseDisplay(display);
    return screen_offset;
}

cv::Mat captureScreenRegion(int x, int y, int width, int height) {
    Display* display = XOpenDisplay(nullptr);
    Window root = DefaultRootWindow(display);
    XImage* img = XGetImage(display, root, x, y, width, height, AllPlanes, ZPixmap);

    cv::Mat mat(height, width, CV_8UC4, img->data);  // BGRA image
    if (mat.channels() == 4) {
        cv::cvtColor(mat, mat, cv::COLOR_BGRA2BGR);
    }

    std::vector<cv::Mat> channels(3);
    cv::split(mat, channels);
    cv::Mat redMat = channels[2];  // Red channel is the third one (index 2)

#ifdef DEBUG
    cv::imshow("Processed Frame", redMat);
    cv::waitKey(1);
#endif
    XDestroyImage(img);
    XCloseDisplay(display);

    return redMat;
}

std::string extractTextFromImage(const cv::Mat& image) {
    tesseract::TessBaseAPI tess;
    tess.Init(NULL, "eng", tesseract::OEM_LSTM_ONLY);
    tess.SetPageSegMode(tesseract::PSM_SINGLE_BLOCK);

    cv::Mat gray;
    if (image.channels() > 1) {
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = image.clone();
    }
    tess.SetImage(gray.data, gray.cols, gray.rows, 1, gray.step);

    char* outText = tess.GetUTF8Text();
    std::string text(outText);
    delete[] outText;

    return normalizeText(text);
}

// Function to check if the text indicates a disconnection
bool isDisconnectionText(const std::string& text) {
    // If the text is too short or doesn't contain "day", it's likely noise
    if (text.length() < 5 || text.find("day") == std::string::npos) {
        return true;
    }
    return false;
}

int main() {
    sendToDiscordWithRetry("Starting GAMMA Simple Logger...");
    int rightMonitorX = getRightMonitorXOffset();  // Detect right monitor position
    cv::Rect captureRegion(rightMonitorX + 770, 215, 375, 45);  // Adjust X coordinate

    bool isDisconnected = false;

    while (true) {
        cv::Mat capturedFrame = captureScreenRegion(
            captureRegion.x, captureRegion.y, captureRegion.width, captureRegion.height);

        if (capturedFrame.empty()) {
            std::cerr << "Failed to capture screen." << std::endl;
            continue;
        }

        std::string currentText = extractTextFromImage(capturedFrame);

#ifdef DEBUG
        std::cout << "Extracted Text: " << currentText << std::endl;
#endif

        // Check if the text indicates a disconnection
        if (isDisconnectionText(currentText)) {
            if (!isDisconnected) {
                sendToDiscordWithRetry("Disconnected from the game.");
                isDisconnected = true;
            }
            continue;
        } else {
            isDisconnected = false;
        }

        // Send the log only if the time (HHMMSS) is new
        if (isNewMessage(currentText)) {
            sendToDiscordWithRetry(currentText);
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return 0;
}
