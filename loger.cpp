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
#include <vector>
#include <deque>

#define DEBUG

std::unordered_set<std::string> recentLogs; // Track full log content to avoid duplicates
std::unordered_set<std::string> sentTimestamps; // Track timestamps to avoid sending the same timestamp again
std::deque<std::string> logBuffer; // Buffer to store recent logs for clearing old entries
const int MAX_LOG_BUFFER_SIZE = 100; // Maximum number of logs to keep in the buffer

// Function to send a message to Discord with retries
bool sendToDiscordWithRetry(const std::string& message, int maxRetries = 3) {
    CURL* curl;
    CURLcode res;
    bool success = false;

    for (int retry = 0; retry < maxRetries; retry++) {
        curl = curl_easy_init();
        if (curl) {
            const std::string webhook_url = "https://discordapp.com/api/webhooks/1336647048367177789/4v_Za4HGbhqO9devyeq0Z3lIVCOGHUwwOLfFzRUvrwhiySIn6onzq5mlomHMeBwuL0kk";

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

// Function to fix concatenated "day" issue
std::string fixConcatenatedDay(const std::string& text) {
    std::string fixedText = text;
    size_t pos = 0;

    while ((pos = fixedText.find("day", pos)) != std::string::npos) {
        // Check if "day" is concatenated with the previous character
        if (pos > 0 && std::isalnum(fixedText[pos - 1])) {
            fixedText.insert(pos, " "); // Insert a space before "day"
            pos += 4; // Move past the inserted space and "day"
        } else {
            pos += 3; // Move past "day"
        }
    }

    return fixedText;
}

// Function to split text into individual logs based on "Day" keyword
std::vector<std::string> splitLogs(const std::string& text) {
    std::vector<std::string> logs;
    size_t startPos = 0;
    size_t dayPos;

    while ((dayPos = text.find("day", startPos)) != std::string::npos) {
        if (dayPos == std::string::npos) break; // No more logs found

        // Find the end of the current log (next "day" or end of text)
        size_t nextDayPos = text.find("day", dayPos + 3);
        if (nextDayPos == std::string::npos) {
            logs.push_back(text.substr(dayPos)); // Last log
            break;
        }

        // Extract the log entry
        logs.push_back(text.substr(dayPos, nextDayPos - dayPos));
        startPos = nextDayPos;
    }

    return logs;
}

// Function to get only the most recent log (the first log detected)
std::string getLatestLog(const std::string& text) {
    // Split the logs based on "day" as you did before
    std::vector<std::string> logs = splitLogs(text);

    // Return the first log entry (most recent)
    if (!logs.empty()) {
        return logs.front();  // Only return the first log entry
    }

    return ""; // If no logs found, return an empty string
}

// Function to extract timestamp from a log
std::string extractTimestamp(const std::string& log) {
    size_t pos = log.find("day");
    if (pos != std::string::npos) {
        return log.substr(pos, 15);  // Extract timestamp in the format "day <num> <time>"
    }
    return "";
}

// Function to check if the log is new based on its content and timestamp
bool isNewLog(const std::string& log) {
    // Extract timestamp and check if it was already sent
    std::string timestamp = extractTimestamp(log);
    if (!timestamp.empty() && sentTimestamps.count(timestamp)) {
        return false; // Log with the same timestamp has already been sent
    }

    // Add the timestamp to the sentTimestamps set to avoid future duplicates
    sentTimestamps.insert(timestamp);

    // Add the log to the recent logs set
    recentLogs.insert(log);
    logBuffer.push_back(log);

    // Clear old logs if the buffer exceeds the maximum size
    if (logBuffer.size() > MAX_LOG_BUFFER_SIZE) {
        std::string oldestLog = logBuffer.front();
        logBuffer.pop_front();
        std::string oldestTimestamp = extractTimestamp(oldestLog);
        if (!oldestTimestamp.empty()) {
            sentTimestamps.erase(oldestTimestamp);  // Remove timestamp from the set
        }
        recentLogs.erase(oldestLog);
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
        cv::cvtColor(mat, mat, cv::COLOR_BGRA2BGR); // Convert to BGR (3 channels)
    }

    // Split the image into its color channels (B, G, R)
    std::vector<cv::Mat> channels(3);
    cv::split(mat, channels);

    // Extract the red and green channels
    cv::Mat redMat = channels[2];  // Red channel is the third one (index 2)
    cv::Mat greenMat = channels[1]; // Green channel is the second one (index 1)

    // Combine the red and green channels
    cv::Mat combinedMat;
    cv::addWeighted(redMat, 0.5, greenMat, 0.5, 0, combinedMat);

#ifdef DEBUG
    cv::imshow("Processed Frame (Red + Green Channels)", combinedMat);
    cv::waitKey(1);
#endif

    XDestroyImage(img);
    XCloseDisplay(display);

    return combinedMat;
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
    sendToDiscordWithRetry("Starting GAMMA Simple Logger...");
    int rightMonitorX = getRightMonitorXOffset();  // Detect right monitor position
    cv::Rect captureRegion(rightMonitorX + 770, 215, 375, 500);  // Capture a larger region to include all logs

    while (true) {
        cv::Mat capturedFrame = captureScreenRegion(
            captureRegion.x, captureRegion.y, captureRegion.width, captureRegion.height);

        if (capturedFrame.empty()) {
            std::cerr << "Failed to capture screen." << std::endl;
            continue;
        }

        std::string allText = extractTextFromImage(capturedFrame);

        // Fix concatenated "day" issue
        allText = fixConcatenatedDay(allText);

#ifdef DEBUG
        std::cout << "Extracted Text: " << allText << std::endl;
#endif

        // Get only the most recent log (first detected in the text)
        std::string latestLog = getLatestLog(allText);

        if (!latestLog.empty() && isNewLog(latestLog)) {
            // Send log to Discord only if it is new
            sendToDiscordWithRetry(latestLog); 
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return 0;
}
