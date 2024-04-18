#include <RF24/RF24.h>
#include <iostream>
#include <thread>
#include <iostream>
#include <fstream>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <fcntl.h>
#include <string.h>
#include <tins/tins.h>

// PINS on the Buses connected to the raspberry -----------------------------------------------------
#define RADIO_ONE_CE_PIN 17
#define RADIO_ONE_CSN_PIN 0
#define RADIO_TWO_CE_PIN 27
#define RADIO_TWO_CSN_PIN 10
// name of the virtual interface created
#define I_FACE "tun0"
// buffer where we store the packet from interface (iface mtu set to 1500 ... this should be enough)
#define BUFFER_SIZE 2048
// --------------------------------------------------------------------------------------------------

// the interface is set up by the program, there should not be one with the same name already existing
// because the program sets up the interface, it must be executed as sudo

const uint8_t addressWidth = 3;

const uint8_t addressMobile[4] = "MOB";
const uint8_t addressBase[4] = "BAS";

// Function to set up the radio for sending
void setupSendRadio(RF24& radio, bool baseStation) {
    radio.begin();
    radio.setPALevel(RF24_PA_LOW);
    radio.setDataRate(RF24_2MBPS);
    radio.setAddressWidth(addressWidth);
    if(baseStation) {
        radio.openWritingPipe(addressMobile); // address, used in the header, outgoing traffic contains this address (to whom?)
        radio.setChannel(76);
    } else {
        radio.openWritingPipe(addressBase);
        radio.setChannel(100);
    }
    
}

// Function to set up the radio for receiving
void setupReceiveRadio(RF24& radio, bool baseStation) {
    radio.begin();
    radio.setPALevel(RF24_PA_LOW);
    radio.setDataRate(RF24_2MBPS);
    radio.setAddressWidth(addressWidth);
    if(baseStation) {
        radio.openReadingPipe(1, addressBase); // address of the listening pipe which will be opened (our address?)
        radio.setChannel(100);
    } else {
        radio.openReadingPipe(1, addressMobile);
        radio.setChannel(76);
    }
    radio.startListening();
}

bool process_received_packet(const uint8_t* packet_data, ssize_t packet_size, uint32_t* actualSize) {
    std::cout << "Packet contents:" << std::endl;
    for (std::size_t i = 0; i < packet_size; i++)
        std::cout << +packet_data[i] << " ";
    std::cout << std::endl;
    try {
        // Parse the raw packet data
        Tins::RawPDU raw_packet(packet_data, packet_size);

        // Serialize the raw packet data
        std::vector<uint8_t> serialized_data = raw_packet.serialize();

        // Get a pointer to the serialized packet data
        const uint8_t* data = serialized_data.data();

        // Check if the first byte corresponds to an IPv4 packet
        if (data && (data[0] >> 4) == 4) {
            // It's an IPv4 packet
            // Cast the raw PDU to an IP object
            const Tins::IP& ip = raw_packet.to<Tins::IP>();

            // Print IP header information
            std::cout << "Source IP: " << ip.src_addr() << std::endl;
            std::cout << "Destination IP: " << ip.dst_addr() << std::endl;
            *actualSize = ip.size();
            // Print payload data size
            const Tins::RawPDU* payload = ip.find_pdu<Tins::RawPDU>();
            if (payload) {
                std::cout << "Payload: " << payload->payload().size() << " bytes" << std::endl;
            }
            return true;
        } else {
            std::cerr << "Not an IPv4 packet" << std::endl;
            return false;
        }
    } catch (const std::exception& ex) {
        std::cerr << "Error parsing packet: " << ex.what() << std::endl;
        return false;
    }
    std::cout << std::endl;
}

// Function to send data
void sendData(RF24& radio, int tun_fd) {
    uint8_t buffer[BUFFER_SIZE];
    uint8_t currentMsg[32];
    while (true) {
        // first we read a packet from the interface:
        ssize_t bytes_read = read(tun_fd, buffer, BUFFER_SIZE);
        if (bytes_read < 0) {
            perror("Failed to read from TUN device");
            return;
        }
        // check that the packet is ip packet
        std::cout << "Read from interface: " << std::endl;
        uint32_t actualSize = 0;
        if(!process_received_packet(buffer, bytes_read, &actualSize)) {
            continue;
        }
        uint8_t seq = 0;
        int index = 0;
        for(; index < bytes_read; ++seq, index+=31) {
            currentMsg[0] = seq;
            int cap = bytes_read - index;
            if(cap > 31) {
                cap = 31;
            }
            for(int i = 0; i < cap; ++i) {
                currentMsg[i+1] = buffer[index+i];
            }
            if(!radio.write(currentMsg, cap+1)) {
                std::cerr << "Failed to send part of the ip packet (fragment)." << std::endl;
            }
        }
        // after sending the whole ip packet, we will send a a message indicating that the whole packet has been sent
        currentMsg[0] = 0;
        radio.write(currentMsg, 1);
    }
}

// Function to receive data
void receiveData(RF24& radio, int tun_fd) {
    uint8_t buffer[BUFFER_SIZE] = {};       // initialize with 0 value
    uint8_t currentMsg[32] = {0};
    bool readingMessage = false;
    uint8_t max = 0;
    while (true) {
        if (radio.available()) {
            radio.read(&currentMsg, 32);
            uint8_t seq = currentMsg[0];
            if(seq == 0) {
                if(readingMessage) { // we got payload with seq num = 0 while reading message, indicating end -> packet is whole
                    // send the data to interface
                    std::cout << "Received by radio, will be sent back to interface: " << std::endl;
                    uint32_t actualSize = 0;
                    if(!process_received_packet(buffer, (max+1)*31, &actualSize)) {
                        readingMessage = false;
                        uint8_t buffer[BUFFER_SIZE] = {}; // reset the values of the buffer to 0;
                        max = 0;
                        continue;
                    }
                    ssize_t bytes_written = write(tun_fd, buffer, actualSize);
                    if (bytes_written < 0) {
                        perror("Failed to write to TUN device");
                    }
                    readingMessage = false;
                    uint8_t buffer[BUFFER_SIZE] = {};
                    max = 0;
                    continue;
                } else {             // we got payload with seq num = 0 while not reading message => first fragment of ip packet
                    readingMessage = true;
                }
            }
            int bufferIndex = seq*31;
            for(int i = 0; i < 31; ++i) {
                buffer[bufferIndex+i] = currentMsg[i+1];
            }
            if(seq > max) {
                max = seq;
            }
        }
    }
}

bool configBaseStation() {
    // masquerade nat ... in postroutign chain we replace source ip addr of outgoing packet with with ip addr of tun0 int.
    int ret = system("iptables -t nat -A POSTROUTING -o eth0 -j MASQUERADE");
    if (ret != 0) {
        std::cerr << "Failed to add MASQUERADE rule with iptables" << std::endl;
        return false;
    }
    // matches packets that are related or part of established connection -> forward from eth0 to tun0 interface
    ret = system("iptables -A FORWARD -i eth0 -o tun0 -m state --state RELATED,ESTABLISHED -j ACCEPT");
    if (ret != 0) {
        std::cerr << "Failed to add FORWARD rule from eth0 to tun0 interface" << std::endl;
        return false;
    }
    // forwards all packets from coming from tun0 interface to eth0 interface (as outgoing int)
    ret = system ("iptables -A FORWARD -i tun0 -o eth0 -j ACCEPT");
    if (ret != 0) {
        std::cerr << "Failed to add FORWARD rule from tun0 to eth0 interface" << std::endl;
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    // setup radios -----------------------------------------------------------------------------------------
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
        return 1;
    }

    RF24 radioSend(RADIO_ONE_CE_PIN, RADIO_ONE_CSN_PIN);
    RF24 radioReceive(RADIO_TWO_CE_PIN, RADIO_TWO_CSN_PIN);

    setupSendRadio(radioSend, baseStation);
    setupReceiveRadio(radioReceive, baseStation);
    
    // setup interface --------------------------------------------------------------------------------------
    int tun_fd = open("/dev/net/tun", O_RDWR);
    if (tun_fd < 0) {
        perror("Failed to open TUN device");
        return 1;
    }
    // setting interfac flags
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    //ifr.ifr_mtu = 1500;                     // set the MTU size to 1500 bytes
    strncpy(ifr.ifr_name, I_FACE, IFNAMSIZ);
    // initializing the interface with set flags
    if (ioctl(tun_fd, TUNSETIFF, (void *)&ifr) < 0) {
        perror("Failed to ioctl TUNSETIFF"); // Print error message
        std::cerr << "Error number: " << errno << std::endl; // Print error number
        close(tun_fd);
        return 1;
    }
    // setting interface owner
    int uid = 1000;
    if (ioctl(tun_fd, TUNSETOWNER, uid) < 0) {
        perror("Failed to set TUN owner");
        close(tun_fd);
        return 1;
    }
    // Assign an IP address to the tun0 interface using ip command
    int ret;
    if(baseStation){
        ret = system("ip addr add 192.168.2.1/24 dev tun0");
    } else {
        ret = system("ip addr add 192.168.2.2/24 dev tun0");
    }
    if (ret != 0) {
        perror("Failed to assign IP address to tun0");
        return 1;
    }
    // changing the state of the interface to up
    ret = system("ip link set dev tun0 up");
    if (ret != 0) {
        perror("Failed to bring interface tun0 up");
        return 1;
    }
    if(baseStation) {   // base station -> forwarding has to be enabled on the system, then we configure...
        if(!configBaseStation()) {
            std::cerr << "Failed to configure the base station.";
            return 1;
        }
    } else {            // mobile station -> default gateway, where we send through tun0 device... 
        ret = system("ip route add default via 192.168.2.1 dev tun0");
        if (ret != 0) {
            std::cerr << "Failed to add default route" << std::endl;
            return 1;
        }
    }
    // ------------------------------------------------------------------------------------------------------

    // Start sender and receiver threads
    std::thread sender(sendData, std::ref(radioSend), tun_fd);
    std::thread receiver(receiveData, std::ref(radioReceive), tun_fd);

    // Join threads to main thread
    sender.join();
    receiver.join();

    close(tun_fd);
    return 0;
}

