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
#include <cmath>
//#include <atomic>

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

#define DEBUGGING false

// the interface is set up by the program, there should not be one with the same name already existing
// because the program sets up the interface, it must be executed as sudo

const uint8_t addressWidth = 3;

const uint8_t addressMobile[4] = "MOB";
const uint8_t addressBase[4] = "BAS";

// variables for some "statistics"
int hadToResend = 0;
int allSent = 0;

// Function to set up the radio for sending
void setupSendRadio(RF24& radio, bool baseStation) {
    radio.begin();
    radio.setPALevel(RF24_PA_LOW);
    radio.setDataRate(RF24_2MBPS);
    radio.setAddressWidth(addressWidth);
    radio.setAutoAck(false);
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
    radio.setAutoAck(false);
    if(baseStation) {
        radio.openReadingPipe(1, addressBase); // address of the listening pipe which will be opened (our address?)
        radio.setChannel(100);
    } else {
        radio.openReadingPipe(1, addressMobile);
        radio.setChannel(76);
    }
    radio.startListening();
}

// function that checks, if the given packet_data form a proper ip packet
bool process_received_packet(const uint8_t* packet_data, ssize_t packet_size) {
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
            return true;
        } else {
            return false;
        }
    } catch (const std::exception& ex) {
        std::cerr << "Error parsing packet: " << ex.what() << std::endl;
        return false;
    }
}

// Function to send data
void sendData(RF24& radio, int tun_fd, int fragmentList[], bool& sendingAltBool) {
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
        if(!process_received_packet(buffer, bytes_read)) {
            continue;
        }

        if(DEBUGGING)
            std::cout << "[SENDING FUNCTION]: Sending ip packet from interface!" << std::endl;

        // calculate how many fragments will be needed to transfer the ip packet (sent in the first msg)
        uint8_t fragmentsToSend = static_cast<uint8_t>(std::ceil(static_cast<double>(bytes_read) / 31.0));
        allSent += fragmentsToSend + 1;     //+1 for the start message, which is always sent

        // first we send the start msg:
        uint8_t startMsg[4];
        startMsg[0] = sendingAltBool ? 0x40 : 0;    // first byte is header, it is either 01000000 or 00000000, first bit = 0 => data, last 6 bits = 0 => seq number
        startMsg[1] = fragmentsToSend;              // second byte is number of fragments of the ip packet being sent
        uint16_t tmpNum = static_cast<uint16_t>(bytes_read);
        // third and fourth bytes contain the number represening the size of the whole ip packet in bytes (as interface mtu = 1500) two bytes is enough (2^16...)
        startMsg[2] = tmpNum >> 8;    // we want the more significant byte here
        startMsg[3] = tmpNum & 0xFF;  // it is the same as doing = tmpNum, as we want the least significant byte, but this is clearer
        radio.write(startMsg, 4);
        
        if(DEBUGGING)
            std::cout << "[SENDING FUNCTION]: Start msg sent, with sendingAltBool: " << sendingAltBool << std::endl;
        
        // then we send the actual data:
        uint8_t seq = 1;
        int index = 0;
        for(; index < bytes_read; ++seq, index+=31) {
            
            if(DEBUGGING)
                std::cout << "[SENDING FUNCTION]: Sending fragment with seq = " << static_cast<int>(seq) << std::endl;
            
            currentMsg[0] = sendingAltBool ? 0x40 : 0;
            currentMsg[0] += seq;
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
        // after sending the whole ip packet once, we check and if needed try again, untill all the acknowledgements have been received
        bool someAckNotReceived = true;
        // here we could implement the max number of tries to resend
        while (someAckNotReceived) {
            // lets wait for one millisecond, to catch up on acknowledgements ... the time could be tweaked (1ms worked pretty well in my ping tests)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            someAckNotReceived = false;
            // first we check if the startMsg acknowledgement has been received
            if(fragmentList[0] != 1) {
                radio.write(startMsg, 4);
                someAckNotReceived = true;
                ++hadToResend;

                if(DEBUGGING)
                    std::cout << "[SENDING FUNCTION]: Had to resend the starting msg" << std::endl;
            
            }
            // then we check all the data fragments
            for(int seq = 1; seq <= fragmentsToSend; ++seq) {
                // means that an acknowledgement as not been received
                if(fragmentList[seq] != 1) {

                    if(DEBUGGING)
                        std::cout << "Had to resend fragment with seq: " << seq << std::endl;

                    someAckNotReceived = true;
                    int index = (seq-1)*31;
                    int cap = bytes_read - index;
                    if(cap > 31) {
                        cap = 31;
                    }
                    currentMsg[0] = sendingAltBool ? 0x40 : 0;
                    currentMsg[0] += seq;
                    for(int i = 0; i < cap; ++i) {
                        currentMsg[i+1] = buffer[index+i];
                    }
                    if(!radio.write(currentMsg, cap+1)) {
                        std::cerr << "Failed to resend part of the ip packet (fragment)." << std::endl;
                    }
                    ++hadToResend;
                }
            }
        }
        // after all the acknowledgements have been received we print out current stats
        //std::cout << "Total messages sent with radios: " << allSent << ", had to resend: " << hadToResend << std::endl;
        // after we have received all the acknowledgements, we can alternate the bool, thus the bit in the header for next ip packet
        sendingAltBool = !sendingAltBool;
        
        if(DEBUGGING)
            std::cout << "[SENDING FUNCTION]: All acknowledgements received, sendingAltBool now: " << sendingAltBool << std::endl;

        // and we have to reset the fragmentList array
        for(int i = 0; i < 64 ; ++i) {
            fragmentList[i] = 0;
        }
    }
}

// Function to receive data
void receiveData(RF24& radioReceive, RF24& radioSend, int tun_fd, int fragmentList[], bool& sendingAltBool) {
    uint8_t buffer[BUFFER_SIZE] = {};       // initialize with 0 value
    uint8_t currentMsg[32] = {0};
    
    bool startReceived = false;         // bool, which tells if we've received the startMsg for the ip packet
    uint8_t fragmentsReceived = 0;      // current number of fragments received
    uint8_t fragmentsToReceive = 0;     // number received in the startMsg, telling us how many data fragments will be received
    uint16_t currentPacketSize = 0;     // number received in the startMsg, telling us how large is the ip packet (in bytes)
    bool newFragments[64];              // array where store info, if the fragment with this seq number has already been received or not
    for (int i = 0; i < 64; ++i) {      // initially we set it all to true meaning, they have not yet been received (they are new)
        newFragments[i] = true;
    }

    bool receivingAltBool = true;       // bool for the alternating bit in our headers (to sync with other station...)

    // the main receiving loop
    while (true) {
        // we wait for a message, and after it arrives, we read it
        if (radioReceive.available()) {
            radioReceive.read(&currentMsg, 32);
            // the first byte is the header
            uint8_t header = currentMsg[0];
            // second most significant bit is the alternating bit between ip packets, all fragments of the packet share same bit
            bool receivedAltBool = (header & 0x40) != 0;    // if the second most significant bit is 1 -> true
            
            if(DEBUGGING)
                std::cout << "[RECEIVING FUNCTION]: Received: most significant bit = " << (header & 0x80) << "; second most = " << (header & 0x40) << "; seq = " << (header & 0x3F) << std::endl;
            
            // if the most significant bit is 1 -> it is acknowledgement
            if((header & 0x80) != 0) {
                // this should theoretically not happen
                if(receivedAltBool != sendingAltBool) {

                    if(DEBUGGING)
                        std::cerr << "[RECEIVING FUNCTION]: Received acknowledgement to previous (old) ip packet" << std::endl;

                // means that we received ack to message that we've sent (that it was received)
                } else {
                    fragmentList[header & 0x3F] = 1;    // we change the value on the index of seq number in the list to 1, means ack received
                }
                continue;
            // if the most significant bit is 0 -> is data fragment -> first we send acknowledgement
            } else {
                uint8_t ack = header | 0x80;     // change the most significant bit to 1 -> making it ack msg, rest of the header is the same
                
                if(DEBUGGING)
                    std::cout << "[RECEIVING FUNCTION]: Sending: most significant bit = " << (ack & 0x80) << "; second most = " << (ack & 0x40) << "; seq = " << (ack & 0x3F) << std::endl;
                
                radioSend.write(&ack, 1);
                // if received data fragment belongs to the previous ip packet -> we resend the ack, but we dont save the data again -> continue
                if (receivedAltBool != receivingAltBool) {
                    
                    if(DEBUGGING)
                        std::cout << "[RECEIVING FUNCTION]: Data fragment belongs to previous ip packet" << std::endl;
                    
                    continue;
                }
            }
            // we get here only if it is data packet and the receivedAltBool == receivingAltBool
            uint8_t seq = header & 0x3F;    // get the sequence number
            // seq == 0 means, a first msg before this ip packet, containing the amount of fragments that should be received and the actual ip packet size
            if(seq == 0) {
                // if we've already received the start msg, no need to set the values again... (possible to receive multiple times, if ack lost)
                if (!startReceived) {
                    
                    if(DEBUGGING)
                        std::cout << "[RECEIVING FUNCTION]: Received a new starting message." << std::endl;
                    
                    startReceived = true;
                    fragmentsToReceive = currentMsg[1];
                    currentPacketSize = (currentMsg[2] << 8) | currentMsg[3];   // deserialization of the number (larger than one byte can contain)
                } else if(DEBUGGING) {
                
                    std::cout << "[RECEIVING FUNCTION]: Received starting message, which have already been received." << std::endl;
                
                }
                continue;
            }

            // we get here if we received a data packet and receivedAltBool = receivingAltBool + the seq number is not == 0
            // we check if the fragment with this sequence number has already been received
            if(newFragments[seq]) {

                if(DEBUGGING)
                    std::cout << "[RECEIVING FUNCTION]: The received message is a new data fragment" << std::endl;
                
                newFragments[seq] = false;
                // if not we save the data, increment the number of packets received
                int bufferIndex = (seq-1)*31;
                for(int i = 0; i < 31; ++i) {
                    buffer[bufferIndex+i] = currentMsg[i+1];
                }
                ++fragmentsReceived;
                // if we've already received the start msg and if we got all the fragments needed
                if(startReceived && fragmentsReceived == fragmentsToReceive) {

                    if(DEBUGGING)
                        std::cout << "[RECEIVING FUNCTION]: Received last fragment, changing receivingAltBool from: " << receivingAltBool;

                    receivingAltBool = !receivingAltBool;
                    
                    if(DEBUGGING)
                        std::cout << "; to: " << receivingAltBool << std::endl;

                    // first we check if the received fragments put together an actual ip packet
                    if(process_received_packet(buffer, currentPacketSize)) {
                        
                        if(DEBUGGING)
                            std::cout << "[RECEIVING FUNCTION]: Received data form an ip packet." << std::endl;
                        
                        // send the data to interface
                        ssize_t bytes_written = write(tun_fd, buffer, currentPacketSize);
                        if (bytes_written < 0) {
                            perror("Failed to write to TUN device");
                        }
                    } else {
                        perror("Received data are not of an IP packet");
                    }
                    // then we reset all the variables
                    for(int i = 0; i < BUFFER_SIZE; ++i) {
                        buffer[i] = 0;
                    }
                    startReceived = false;
                    fragmentsReceived = 0;
                    fragmentsToReceive = 0; // these two (toReceive and packetSize) may not need a reset, but it is good for debugging purposes
                    currentPacketSize = 0;
                    for (int i = 0; i < 64; ++i) {
                        newFragments[i] = true;
                    }
                }
                // if the start has not been received, or we don't have all the fragments, we just wait for them
            } else if(DEBUGGING) {

                std::cout << "[RECEIVING FUNCTION]: Received data fragment, which have already been received." << std::endl;

            }
            // if the fragment has already been received we don't need to do anything, the ack has already been resent
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
        std::cerr << "Invalid argument: " << arg << "; should be: [--mobile | --base]" << std::endl;
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
    
    // list, that will be shared between the threads, containing info, if an acknowledgement for the fragment has been received or not
    int fragmentList[64] = {};      // size 64 is enough, as interface mtu is 1500 and 64*31 >> 1500

    // bool shared between the threads, that contains info about the value of the second most significant bit in sent msgs, thus in received acks
    bool sendingAltBool = true;

    // Start sender and receiver threads
    std::thread sender(sendData, std::ref(radioSend), tun_fd, fragmentList, std::ref(sendingAltBool));
    std::thread receiver(receiveData, std::ref(radioReceive), std::ref(radioSend), tun_fd, fragmentList, std::ref(sendingAltBool));

    // Join threads to main thread
    sender.join();
    receiver.join();

    close(tun_fd);
    return 0;
}

