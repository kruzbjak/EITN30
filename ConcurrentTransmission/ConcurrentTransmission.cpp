#include <RF24/RF24.h>
#include <iostream>
#include <thread>
#include <chrono>

#define RADIO_ONE_CE_PIN 17
#define RADIO_ONE_CSN_PIN 0
#define RADIO_TWO_CE_PIN 27
#define RADIO_TWO_CSN_PIN 10

const uint8_t addressWidth = 3;

const uint8_t addressMobile[4] = "MOB";
const uint8_t addressBase[4] = "BAS";

// Function to set up the radio for sending
void setupSendRadio(RF24& radio, bool baseStation) {
    radio.begin();
    radio.setPALevel(RF24_PA_LOW);
    radio.setDataRate(RF24_250KBPS);
    radio.setAddressWidth(addressWidth);
    if(baseStation) {
        radio.openWritingPipe(addressMobile); // address, used in the header, outgoing traffic contains this address (to whom?)
    } else {
        radio.openWritingPipe(addressBase);  
    }
    
}

// Function to set up the radio for receiving
void setupReceiveRadio(RF24& radio, bool baseStation) {
    radio.begin();
    radio.setPALevel(RF24_PA_LOW);
    radio.setDataRate(RF24_250KBPS);
    radio.setAddressWidth(addressWidth);
    if(baseStation) {
        radio.openReadingPipe(1, addressBase); // address of the listening pipe which will be opened (our address?)
    } else {
        radio.openReadingPipe(1, addressMobile);
    }
    radio.startListening();
}

// Function to send data
void sendData(RF24& radio, bool baseStation) {
    std::string message;
    if(baseStation) {
        message = "Hello from base station!";
    } else {
        message = "Hello from mobile station!";
    }
    while (true) {
        if (radio.write(message.c_str(), message.length())) {
            std::cout << "Message sent: " << message << std::endl;
        } else {
            std::cerr << "Failed to send message." << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// Function to receive data
void receiveData(RF24& radio, bool baseStation) {
    std::string whoReceived;
    if(baseStation) {
        whoReceived = "Base station received: ";
    } else {
        whoReceived = "Mobile station received: ";
    }
    char buffer[32] = {0};
    while (true) {
        if (radio.available()) {
            radio.read(&buffer, sizeof(buffer));
            std::cout << whoReceived << buffer << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

int main(int argc, char** argv) {
    bool baseStation; // 0 uses address[0] (BAS) to transmit/write, 1 uses address[1] (MOB) to transmit/write
     // Check if at least one command-line argument is provided
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " [--mobile | --base]" << std::endl;
        return 1; // Return error code
    }
    // Convert the command-line argument to a std::string for easier comparison
    std::string arg = argv[1];
    // Check which argument was passed
    if (arg == "--mobile") {
        baseStation = false;
    } else if (arg == "--base") {
        baseStation = true;
    } else {
        std::cerr << "Invalid argument: " << arg << "; shouold be: [--mobile | --base]" << std::endl;
        return 1; // Return error code
    }

    RF24 radioSend(RADIO_ONE_CE_PIN, RADIO_ONE_CSN_PIN);
    RF24 radioReceive(RADIO_TWO_CE_PIN, RADIO_TWO_CSN_PIN);

    setupSendRadio(radioSend, baseStation);
    setupReceiveRadio(radioReceive, baseStation);

    // Start sender and receiver threads
    std::thread sender(sendData, std::ref(radioSend), baseStation);
    std::thread receiver(receiveData, std::ref(radioReceive), baseStation);

    // Join threads to main thread
    sender.join();
    receiver.join();

    return 0;
}