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
#include <X11/extensions/Xinerama.h>  // Needed for multi-monitor detection

#define DEBUG

void sendToDiscord(const std::string& message) {
    CURL* curl;
    CURLcode res;
    curl = curl_easy_init();
//https://discord.com/api/webhooks/1334124265104085044/i4F99g9T1-5rydCr7qbgJ5WHLCPkdpR-098MCv4eFU9BJzwCTvrp7IeIe96_ICTepj5K
    if (curl) {
        const std::string webhook_url = "";

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


// Function to detect multi-monitor setup and get the right screen's position
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

    // Ensure it's in BGR format
    if (mat.channels() == 4) {
        cv::cvtColor(mat, mat, cv::COLOR_BGRA2BGR);
    }

    // Convert to grayscale
    cv::Mat grayMat;
    if (mat.channels() == 3) {
        cv::cvtColor(mat, grayMat, cv::COLOR_BGR2GRAY);
    } else {
        grayMat = mat.clone();  // Already grayscale
    }

    // Optional: Increase contrast for better OCR
    // cv::adaptiveThreshold(grayMat, grayMat, 255, cv::ADAPTIVE_THRESH_MEAN_C, cv::THRESH_BINARY, 11, 2);

    // Debugging: Show the processed image
#ifdef DEBUG
    cv::imshow("Processed Frame", grayMat);
    cv::waitKey(1);
#endif
    XDestroyImage(img);
    XCloseDisplay(display);

    return grayMat;
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

    return text;
}

int main() {
    int rightMonitorX = getRightMonitorXOffset();  // Detect right monitor position
    cv::Rect captureRegion(rightMonitorX + 770, 215, 375, 45);  // Adjust X coordinate

    std::string lastCapturedText;

    while (true) {
        cv::Mat capturedFrame = captureScreenRegion(
            captureRegion.x, captureRegion.y, captureRegion.width, captureRegion.height);

        if (capturedFrame.empty()) {
            std::cerr << "Failed to capture screen." << std::endl;
            continue;
        }


        std::string currentText = extractTextFromImage(capturedFrame);

        // Print extracted text
        std::cout << "Extracted Text: " << currentText << std::endl;

        if (!currentText.empty() && currentText != lastCapturedText) {
            sendToDiscord(currentText);
            lastCapturedText = currentText;
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}
