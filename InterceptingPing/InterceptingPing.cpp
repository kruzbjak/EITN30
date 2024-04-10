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

// the interface is set up by the program, there should not be one with the same name already existing
// because the program sets up the interface, it must be executed as sudo

#define I_FACE "tun0"
#define BUFFER_SIZE 2048

void process_received_packet(const uint8_t* packet_data, ssize_t packet_size) {
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

            // Print payload data
            const Tins::RawPDU* payload = ip.find_pdu<Tins::RawPDU>();
            if (payload) {
                std::cout << "Payload: " << payload->payload().size() << " bytes" << std::endl;
            }
        } else {
            std::cerr << "Not an IPv4 packet" << std::endl;
        }
    } catch (const std::exception& ex) {
        std::cerr << "Error parsing packet: " << ex.what() << std::endl;
    }
}

int main() {
    int tun_fd = open("/dev/net/tun", O_RDWR);
    if (tun_fd < 0) {
        perror("Failed to open TUN device");
        return 1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    strncpy(ifr.ifr_name, I_FACE, IFNAMSIZ);
    std::cout << ifr.ifr_name << std::endl;
    std::cout << I_FACE << std::endl;

    if (ioctl(tun_fd, TUNSETIFF, (void *)&ifr) < 0) {
        perror("Failed to ioctl TUNSETIFF"); // Print error message
        std::cerr << "Error number: " << errno << std::endl; // Print error number
        close(tun_fd);
        return 1;
    }

    int uid = 1000;
    if (ioctl(tun_fd, TUNSETOWNER, uid) < 0) {
        perror("Failed to set TUN owner");
        close(tun_fd);
        return 1;
    }

    // Assign an IP address to the tun0 interface using ip command
    int ret = system("ip addr add 192.168.2.2/24 dev tun0");
    if (ret != 0) {
        perror("Failed to assign IP address to tun0");
        return 1;
    }
    ret = system("ip link set dev tun0 up");
    if (ret != 0) {
        perror("Failed to bring interface tun0 up");
        return 1;
    }

    ret = system("ip route add default via 192.168.2.1 dev tun0");
    if (ret != 0) {
        std::cerr << "Failed to add default route" << std::endl;
        // Handle error or return non-zero to indicate failure
        return 1;
    }
    std::cout << "Default route added successfully" << std::endl;

    while (true) {
        uint8_t buffer[BUFFER_SIZE];
        ssize_t bytes_read = read(tun_fd, buffer, BUFFER_SIZE);
        if (bytes_read < 0) {
            perror("Failed to read from TUN device");
            close(tun_fd);
            return 1;
        }

        process_received_packet(buffer, bytes_read);
    }

    close(tun_fd);
    return 0;
}
