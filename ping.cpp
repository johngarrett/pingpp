#include "ping.hpp"
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h> 

#include <netinet/ip_icmp.h> 
#include <netinet/ip6.h>
#include <netinet/icmp6.h>

#include <iostream>
#include <iomanip>
#include <errno.h>

void ping::start_ping(const std::string&  destination, const Parameters& p) {
    //unsigned short pid = getpid();
    int pid = getpid();
    //std::cout << getpid() << "\t" << pid << "\n";
   while (pid/ 100000 != 0) pid /= 10; // TODO cast to unsigned short, see if that helps
   if (p.verbose) std::cout << " the pid is " << pid << "\n";
    setuid(getuid());
         
    Destination d{destination};
    PingResults results{ping_destination(d, p, pid)};

    std::cout << "\t" << destination << " ping stats \t\n";
    std::cout << results.num_sent << " ICMP echo request packets sent, " << results.num_recv << " ICMP echo reply packets recieved.\n";
    std::cout << std::setw(2) << float(results.num_sent - results.num_recv) / float(results.num_sent) * 100 << "% packet loss.\n";
    std::cout << "Total time: " << std::setw(2) << results.total_time_ms << "ms, avg rtt: " << std::setw(4) << results.avg_rtt << "\n";
}

ping::PingResults ping::ping_destination(const Destination& destination, const Parameters& p, unsigned short id) {
    int sock;
    if (destination.type == IPV6) {
        sock = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6); // protocol (the number that appears in the header (ECHO REPLY/REQ))

        if (sock < 0) {
            close(sock);
            throw std::runtime_error("Couldn't initalize socket. Your machine may require you to run as root.\n");
        }
        icmp6_filter filter;
        int socket_result = setsockopt(sock, // int representation of our socket
                                IPPROTO_ICMPV6, // the level we are operating on
                                ICMP6_FILTER, // the type for the following option
                                (char *)&filter, // the options we are sending over, filters
                                sizeof(filter));

        if (socket_result < 0) { // TODO: throw errno as well
            throw std::runtime_error(std::string("Error number %d occured when trying to manipulate socket filter options\n.", errno ));
        }
        int temp = 2;
        int sock_result = setsockopt(sock,
                                IPPROTO_IPV6,
                                IPV6_MULTICAST_HOPS,
                                (char *) &temp,
                                sizeof(temp));

        if (sock_result < 0) {
           throw std::runtime_error(std::string("Couldnt set hop limit on ipv6. Error number %d", errno));
        }
   } else {
        sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
        if (sock < 0) { 
            close(sock);
            throw std::runtime_error("Couldn't initalize socket. Your machine may require you to run as root.\n");
        }
        // TODO: reinsert filters
          int sock_result = setsockopt(sock,
                                IPPROTO_IP,
                                IP_TTL,
                                (char *) &p.ttl,
                                sizeof(p.ttl));
        if (sock_result < 0) {
            throw std::runtime_error(std::string("Couldnt set ttl options on socket. Error number %d", errno));
        }
    }

    unsigned short num_recieved = 0;
    unsigned short num_sent;
    
    for (num_sent = 0; p.packet_quantity != -1 ? num_sent < p.packet_quantity : 0; num_sent++) { // if packet_quantity == -1, loop infinitely
        if (!p.quiet) std::cout << "attempting ping #" << num_sent + 1 << "\n";
        // time start
        send_imcp_echo_packet(destination, p, sock, num_sent, id);
        num_recieved += listen_for_reply(destination, p, sock, id);
        // time end
    }

    return PingResults{num_sent, num_recieved,0,0};
}

void ping::send_imcp_echo_packet(const Destination& d, const Parameters& p, int sock, unsigned short seq, unsigned short id) {
    char header[d.type == IPV6 ? sizeof(icmp6_hdr) : sizeof(icmphdr)];
    memset(header, 0, sizeof(header)); // fill with zeros to ensure no corruption or anything
    if (p.verbose) std::cout << "Type is " << d.type << " so we are using a packet sizeof " << (d.type == IPV6 ? sizeof(icmp6_hdr) : sizeof(icmphdr)) << "\n";

    if (d.type == IPV6) {
        icmp6_hdr *pkt = (icmp6_hdr *)header; // everything inside icmp6_hdr is an int or struct of ints
        pkt->icmp6_type = ICMP6_ECHO_REQUEST; // ICMP6 ECHO for out, ICMP6_ECHO_REPLY will come back to us
        pkt->icmp6_code = 0; // code 0 = echo reply/req
        pkt->icmp6_id = htons(id & 0xFFFF); // htons converts the unsigned integer destinationlong to network bye order
        pkt->icmp6_seq = seq; 
        pkt->icmp6_cksum = 0;//checksum((uint16_t *) pkt, sizeof(packet));
        memset(pkt->icmp6_dataun.icmp6_un_data8, p.ttl, sizeof(p.ttl));// = p.ttl;
       // TODO I dont think i actually have data, just headers

        //memset((char *)header + sizeof(icmp6_hdr) + sizeof(ICMP6_ECHO_REQUEST), 'I', p.packet_size); // TODO: check this
    } else {
       icmphdr *pkt = (icmphdr *) header; // everything inside icmphdr is an int or struct of ints
       pkt->type = ICMP_ECHO; // ICMP ECHO for out, ICMP_ECHO_REPLY will come back to us
       pkt->code = 0; // code 0 = echo reply/req
       pkt->un.echo.id = htons(id & 0xFFFF); // htons converts the unsigned integer hostlong to network bye order
       pkt->un.echo.sequence = seq;
       pkt->checksum = checksum((uint16_t *) pkt, sizeof(header));// checksum(d);
    
       //memset(header + sizeof(header),  'J', p.packet_size); // set some dummy data
       // TODO I dont think i actually have data, just headers
    }
    
    if(p.verbose) {
        //print packet contents
    }

    int bytes = sendto(sock, header, sizeof(header), 0 /*flags*/, d.get_sock_addr(), d.type == IPV6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in));

    if (bytes < 0) {
       close(sock);
       throw std::runtime_error("[SEND] the sendto command returned a value less than 0.\nWe cannot send to the reciever.\n");
   } else if (bytes != sizeof(header)) {
       close(sock);
       throw std::runtime_error("[SEND] could not write entire packet.\nThere may be an internal size mismatch.\n");
   }
}

unsigned short ping::listen_for_reply(const Destination& d, const Parameters& p, int sock, unsigned short id) {
    while (1) {
            char inbuf[192]; // 100 is an arbitary size, big enough to store a common echo  packet
            memset(inbuf, 0, sizeof(inbuf));
            
            int addrlen = d.type == IPV6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
            int bytes = recvfrom(sock, inbuf /*buffer to save the bytes*/, sizeof(inbuf), 0 /*flags*/, d.get_sock_addr(), (socklen_t *)&addrlen);

            if (bytes < 0) {
                if (p.verbose) std::cout << "[LISTEN] bytes found: " << bytes << "\n\twe expect a value > 0... continuing\n";
                continue;
            }

            if (bytes < int(d.type == IPV6 ? sizeof(icmp6_hdr) : (sizeof(iphdr) + sizeof(icmphdr)))) { 
                if (p.verbose) std::cout << "[LISTEN] Incorrect read bytes! For type " << d.type << "\n\t... continuing\n";
            }

            if (d.type == IPV6) {
                ip6_hdr *iph = (ip6_hdr *)inbuf;  // headerlength is automtically cropped out?????????? 
                icmp6_hdr *pkt = (icmp6_hdr *)(iph); // at this point, inbuf + hlen points to the icmp header
                int extracted_id = ntohs(pkt->icmp6_id); // converts unsigned int from network to destination byte order (opposite of the htons we did earlier! ish)

                if (pkt->icmp6_type == ICMP6_ECHO_REPLY) {
                    if (p.verbose) std::cout << "[LISTEN] packet type of ICMP6 Echo Reply found.\n";
                    if (p.verbose) std::cout << "packet id: " << extracted_id << "\nexpected id: " << id << "\npacket checksum: " << pkt->icmp6_cksum << "\n";

                    if (!p.quiet) {
                        //64 bytes from 192.168.1.1: icmp_seq=1 ttl=64 time=7.68 ms
                       // std::cout << "TODO:TRACK BYTES" << "from " << destination.readable_address() << ": icmp_seq=" << iph->ip6 
                    }

                    return (extracted_id == id);
                } else if (pkt->icmp6_type == ICMP6_DST_UNREACH) {
                    if (p.verbose) std::cout << "[LISTEN] packet type of ICMP6_DEST_UNREACH\n";

                    int offset = sizeof(ip6_hdr) + sizeof(icmp6_hdr) + sizeof(ip6_hdr); // ip6_hdr + icmp hdr going out + another ip6_hdr coming back in? TODO
                    if (((bytes) - offset) == sizeof(icmp6_hdr))
                    {
                        icmp6_hdr *packet = reinterpret_cast<icmp6_hdr *>(inbuf + offset); // extract the original icmp packet
                        if (ntohs(packet->icmp6_id) == id) {
                            if (p.verbose) std::cout << "\tID's match, destination is unreachable\n";
                            return 0;
                        }
                    }
                }
            }  else {
                iphdr *iph = (iphdr *)inbuf;
                int hlen = (iph->ihl << 2); // shift left 2
                bytes -= hlen; // subtract the ip header from the bytes, we only care about the icmp info
                icmphdr *pkt = (icmphdr *)(inbuf + hlen); // at this point, inbuf + hlen points to the icmp header
                int extracted_id = ntohs(pkt->un.echo.id); // converts unsigned int from network to host byte order (opposite of the htons we did earlier! ish)

                if (pkt->type == ICMP_ECHOREPLY) {
                    if (p.verbose) std::cout << "[LISTEN] packet type of ICMP Echo Rely found.\n";
                    if (p.verbose) std::cout << "packet id: " << extracted_id << "\nexpected id: " << id << "\npacket checksum: " << pkt->checksum << "\n";
                    return (extracted_id == id);
                }                
                else if (pkt->type == ICMP_DEST_UNREACH) {
                    std::cout << "[LISTEN] packet type of ICMP_DEST_UNREACH\n";

                    int offset = sizeof(iphdr) + sizeof(icmphdr) + sizeof(iphdr); // iphdr + icmp hdr going out + another iphdr coming back in? TODO
                    if (((bytes + hlen) - offset) == sizeof(icmphdr))
                    {
                        icmphdr *p = reinterpret_cast<icmphdr *>(inbuf + offset); // extract the original icmp packet
                        if (ntohs(p->un.echo.id) == id) {
                            std::cout << "\tID's match, destination is unreachable\n";
                            return 0;
                        }
                    }
                }
            }
            std::cout << "[LISTEN] we got a packet back but it wasnt a reply or an unreachable error...\n";
            return 0;
        }
}

int32_t ping::checksum(const Destination& d, const Parameters& p) {
    /** TODO: chksm for icmpv6 and icmp4 (they differ)
    int32_t nleft = len;
    int32_t sum = 0;
    uint16_t *w = buf;
    uint16_t answer = 0;

    while(nleft > 1)
    {
        sum += *w++;
        nleft -= 2;
    }

    if(nleft == 1)
    {
        *(uint16_t *)(&answer) = *(uint8_t *)w;
        sum += answer;
    }

    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    answer = ~sum;

    return answer;
    */
    return 0;
}
int32_t ping::checksum(uint16_t *buf, int32_t len) {
    int32_t nleft = len;
    int32_t sum = 0;
    uint16_t *w = buf;
    uint16_t answer = 0;

    while(nleft > 1)
    {
        sum += *w++;
        nleft -= 2;
    }

    if(nleft == 1)
    {
        *(uint16_t *)(&answer) = *(uint8_t *)w;
        sum += answer;
    }

    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    answer = ~sum;

    return answer;
}
