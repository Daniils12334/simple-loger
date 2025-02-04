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

#define DEBUG

std::unordered_set<std::string> recentMessages;

void sendToDiscord(const std::string& message) {
    CURL* curl;
    CURLcode res;
    curl = curl_easy_init();
    if (curl) {
        const std::string webhook_url = "ENTER_YOUR_HOOK";

        // Escape newlines and quotes to keep valid JSON format
        std::string json_payload = R"({"content": ")" + message + R"("})";
        json_payload.erase(std::remove(json_payload.begin(), json_payload.end(), '\n'), json_payload.end()); // Remove newlines

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, webhook_url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        res = curl_easy_perform(curl);
        if (res != CURLE_OK)
            std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;

        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
    }
}

std::string normalizeText(const std::string& text) {
    std::string cleaned = text;
    // Remove leading/trailing spaces
    cleaned.erase(0, cleaned.find_first_not_of(" \t\n\r"));
    cleaned.erase(cleaned.find_last_not_of(" \t\n\r") + 1);
    // Convert to lowercase
    std::transform(cleaned.begin(), cleaned.end(), cleaned.begin(), ::tolower);
    // Remove all non-alphanumeric characters except spaces and colons
    cleaned.erase(std::remove_if(cleaned.begin(), cleaned.end(),
                  [](unsigned char c) { return !std::isalnum(c) && c != ' ' && c != ':'; }),
                  cleaned.end());
    // Remove extra spaces
    cleaned.erase(std::unique(cleaned.begin(), cleaned.end(),
                  [](char a, char b) { return a == ' ' && b == ' '; }),
                  cleaned.end());
    return cleaned;
}

// Function to check if two messages are similar (fuzzy matching)
bool areMessagesSimilar(const std::string& message1, const std::string& message2) {
    // If the messages are identical, they are similar
    if (message1 == message2) return true;

    // Calculate the length difference
    size_t len1 = message1.length();
    size_t len2 = message2.length();
    if (std::abs((int)len1 - (int)len2) > 5) return false; // Allow small length differences

    // Count the number of differing characters
    size_t minLen = std::min(len1, len2);
    size_t diffCount = 0;
    for (size_t i = 0; i < minLen; ++i) {
        if (message1[i] != message2[i]) diffCount++;
        if (diffCount > 3) return false; // Allow up to 3 character differences
    }

    return true;
}

// Function to check if the message is new and prevent spam
bool isNewMessage(const std::string& message) {
    // Get the current date
    std::time_t now = std::time(nullptr);
    std::tm* now_tm = std::localtime(&now);
    char dateBuffer[11]; // Buffer to store the date in YYYY-MM-DD format
    std::strftime(dateBuffer, sizeof(dateBuffer), "%Y-%m-%d", now_tm);
    std::string dateStr(dateBuffer);

    // Combine the message and date to create a unique key
    std::string messageKey = message + "|" + dateStr;

    // Check if a similar message already exists
    for (const auto& recentMessage : recentMessages) {
        if (areMessagesSimilar(recentMessage, messageKey)) return false;
    }

    recentMessages.insert(messageKey);
    if (recentMessages.size() > 10) {
        recentMessages.erase(recentMessages.begin()); // Keep a rolling window of last 10 messages
    }
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

int main() {
    sendToDiscord("Starting Simple Logger...");
    int rightMonitorX = getRightMonitorXOffset();  // Detect right monitor position
    cv::Rect captureRegion(rightMonitorX + 770, 215, 375, 45);  // Adjust X coordinate

    std::string lastMessage;
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

        // Check if the game is disconnected (e.g., empty text or specific keywords)
        if (currentText.empty() || currentText.find("disconnected") != std::string::npos) {
            if (!isDisconnected) {
                sendToDiscord("Disconnected from the game.");
                isDisconnected = true;
            }
            continue;
        } else {
            isDisconnected = false;
        }

        // Send the message only if it's new and not a duplicate for the current date
        if (!currentText.empty() && isNewMessage(currentText) && !areMessagesSimilar(currentText, lastMessage)) {
            sendToDiscord(currentText);
            lastMessage = currentText;
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return 0;
}
