#ifndef TRANSACTION_H
#define TRANSACTION_H

#include <string>

struct Transaction {
    int id = 0;
    int cardNo = 0;
    double amount = 0.0;
    std::string merchant;
    std::string location;
    double latitude = 0.0;
    double longitude = 0.0;
    std::string timestamp;            // "YYYY-MM-DD HH:MM:SS"

    // Simulated device identifier (project-level input, NOT real fingerprinting)
    std::string deviceId;

    double distanceFromPrevious = 0.0;

    // Scores
    int prelimScore = 0;              // before OTP step-up
    int finalScore = 0;               // after OTP outcome (0-100)
    int score = 0;                    // kept for older modules = finalScore

    // Authentication
    bool otpRequired = false;
    bool otpVerified = false;
    bool authenticationRequired = false;
    std::string authenticationMethod; // "None" / "OTP"
    std::string otpStatus = "NOT REQUIRED";   // NOT REQUIRED / VERIFIED / FAILED
    std::string authStatus = "NOT REQUIRED";  // NOT REQUIRED / SUCCESSFUL / FAILED
    int failedAuthAttempts = 0;

    // Outcome
    std::string fraudStatus = "NORMAL";       // NORMAL / SUSPICIOUS / HIGH RISK / CRITICAL FRAUD
    std::string decision = "ALLOWED";         // ALLOWED / BLOCKED
    bool blocked = false;
    bool flagged = false;
    std::string flagTimestamp;
    std::string reasons;                      // ';' separated list of triggered rules
};

#endif
