#ifndef _WIN32

#include "prober.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/icmp6.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <poll.h>
#include <linux/errqueue.h>
#include <cstring>
#include <chrono>
#include <vector>
#include <atomic>

namespace mtr {

namespace {

uint16_t computeChecksum(void* b, int len) {
    uint16_t* buf = (uint16_t*)b;
    uint32_t sum = 0;
    for (sum = 0; len > 1; len -= 2) {
        sum += *buf++;
    }
    if (len == 1) {
        sum += *(uint8_t*)buf;
    }
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    return ~sum;
}

// 从 Linux IP_RECVERR / IPV6_RECVERR (MSG_ERRQUEUE) 扩展错误队列中提取属于本探测包 (expected_seq) 的中间跳路由器 IP (TTL/HopLimit Expired)
bool readErrorQueueRouterIP(int sock, uint16_t expected_seq, int expected_ttl, bool is_udp, bool is_v6, std::string& out_ip) {
    char buf[1024];
    char cbuf[512];
    struct iovec iov;
    struct msghdr msg;
    struct sockaddr_storage target;

    iov.iov_base = buf;
    iov.iov_len = sizeof(buf);

    std::memset(&msg, 0, sizeof(msg));
    msg.msg_name = &target;
    msg.msg_namelen = sizeof(target);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);

    ssize_t res = recvmsg(sock, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
    if (res < 0) return false;

    // 严格核对内层触发 ICMP/ICMPv6 错误的原始数据报文，确保匹配本线程的序列号或端口
    bool matched = false;
    if (!is_udp) {
        if (is_v6) {
            if (res >= (ssize_t)sizeof(struct icmp6_hdr)) {
                struct icmp6_hdr* icmp6 = (struct icmp6_hdr*)buf;
                if (ntohs(icmp6->icmp6_seq) == expected_seq) {
                    matched = true;
                }
            }
            if (!matched && res >= (ssize_t)(40 + sizeof(struct icmp6_hdr))) {
                // 内层包含 40 字节 IPv6 头部
                struct icmp6_hdr* inner_icmp6 = (struct icmp6_hdr*)(buf + 40);
                if (ntohs(inner_icmp6->icmp6_seq) == expected_seq) {
                    matched = true;
                }
            }
        } else {
            if (res >= (ssize_t)sizeof(struct icmphdr)) {
                struct icmphdr* icmp = (struct icmphdr*)buf;
                if (ntohs(icmp->un.echo.sequence) == expected_seq) {
                    matched = true;
                }
            }
            if (!matched && res >= (ssize_t)(sizeof(struct iphdr) + sizeof(struct icmphdr))) {
                struct iphdr* ip = (struct iphdr*)buf;
                int ip_len = ip->ihl * 4;
                if (res >= ip_len + (ssize_t)sizeof(struct icmphdr)) {
                    struct icmphdr* inner_icmp = (struct icmphdr*)(buf + ip_len);
                    if (ntohs(inner_icmp->un.echo.sequence) == expected_seq) {
                        matched = true;
                    }
                }
            }
        }
    } else {
        uint16_t expected_port = static_cast<uint16_t>(33434 + expected_ttl);
        if (res >= (ssize_t)sizeof(struct udphdr)) {
            struct udphdr* udp = (struct udphdr*)buf;
            if (ntohs(udp->dest) == expected_port) {
                matched = true;
            }
        }
        if (!matched) {
            int ip_len = is_v6 ? 40 : (res >= 20 ? ((struct iphdr*)buf)->ihl * 4 : 0);
            if (ip_len > 0 && res >= ip_len + (ssize_t)sizeof(struct udphdr)) {
                struct udphdr* inner_udp = (struct udphdr*)(buf + ip_len);
                if (ntohs(inner_udp->dest) == expected_port) {
                    matched = true;
                }
            }
        }
    }

    if (!matched) return false;

    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if ((cmsg->cmsg_level == SOL_IP && cmsg->cmsg_type == IP_RECVERR) ||
            (cmsg->cmsg_level == SOL_IPV6 && cmsg->cmsg_type == IPV6_RECVERR)) {
            struct sock_extended_err* ee = (struct sock_extended_err*)CMSG_DATA(cmsg);
            if (ee && (ee->ee_origin == SO_EE_ORIGIN_ICMP || ee->ee_origin == SO_EE_ORIGIN_ICMP6 || ee->ee_origin == SO_EE_ORIGIN_LOCAL)) {
                if (is_v6) {
                    struct sockaddr_in6* offender = (struct sockaddr_in6*)SO_EE_OFFENDER(ee);
                    if (offender && offender->sin6_family == AF_INET6) {
                        char ip_str[INET6_ADDRSTRLEN];
                        inet_ntop(AF_INET6, &(offender->sin6_addr), ip_str, INET6_ADDRSTRLEN);
                        out_ip = ip_str;
                        return true;
                    }
                } else {
                    struct sockaddr_in* offender = (struct sockaddr_in*)SO_EE_OFFENDER(ee);
                    if (offender && offender->sin_family == AF_INET) {
                        char ip_str[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET, &(offender->sin_addr), ip_str, INET_ADDRSTRLEN);
                        out_ip = ip_str;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

} // namespace

bool RouteProber::resolveHost(const std::string& host, std::string& out_ip, int family_preference) {
    if (host.empty()) return false;

    std::string clean_host = host;
    if (clean_host.length() >= 2 && clean_host.front() == '[' && clean_host.back() == ']') {
        clean_host = clean_host.substr(1, clean_host.length() - 2);
    }

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    if (family_preference == 6) {
        hints.ai_family = AF_INET6;
    } else if (family_preference == 4) {
        hints.ai_family = AF_INET;
    } else {
        if (clean_host.find(':') != std::string::npos) {
            hints.ai_family = AF_INET6;
        } else {
            hints.ai_family = AF_UNSPEC;
        }
    }
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    if (getaddrinfo(clean_host.c_str(), nullptr, &hints, &res) != 0 || !res) {
        if (hints.ai_family == AF_INET6 && family_preference == 0) {
            hints.ai_family = AF_UNSPEC;
            if (getaddrinfo(clean_host.c_str(), nullptr, &hints, &res) != 0 || !res) {
                return false;
            }
        } else {
            return false;
        }
    }

    struct addrinfo* chosen = res;
    if (family_preference == 6) {
        for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
            if (p->ai_family == AF_INET6) { chosen = p; break; }
        }
    } else if (family_preference == 4) {
        for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
            if (p->ai_family == AF_INET) { chosen = p; break; }
        }
    }

    if (chosen->ai_family == AF_INET6) {
        struct sockaddr_in6* ipv6 = (struct sockaddr_in6*)chosen->ai_addr;
        char ip_str[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, &(ipv6->sin6_addr), ip_str, INET6_ADDRSTRLEN);
        out_ip = ip_str;
    } else {
        struct sockaddr_in* ipv4 = (struct sockaddr_in*)chosen->ai_addr;
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(ipv4->sin_addr), ip_str, INET_ADDRSTRLEN);
        out_ip = ip_str;
    }

    freeaddrinfo(res);
    return true;
}

ProbeRecord RouteProber::probeSingleHop(const std::string& target_ip, int ttl, int timeout_ms, int payload_size) {
    ProbeRecord record;
    record.ttl = ttl;
    record.timestamp = std::chrono::system_clock::now();

    bool is_v6 = (target_ip.find(':') != std::string::npos);

    static std::atomic<uint16_t> seq_counter{1};
    uint16_t probe_seq = static_cast<uint16_t>((ttl << 8) | (seq_counter.fetch_add(1) & 0xFF));
    uint16_t probe_id = static_cast<uint16_t>((getpid() & 0xFFFF) ^ ttl);

    int sock = -1;
    bool is_dgram_icmp = false;
    bool is_raw_icmp = false;
    bool is_udp_trace = false;

    if (is_v6) {
        sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_ICMPV6);
        if (sock >= 0) is_dgram_icmp = true;
        if (sock < 0) {
            sock = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
            if (sock >= 0) is_raw_icmp = true;
        }
        if (sock < 0) {
            sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
            if (sock >= 0) is_udp_trace = true;
        }
    } else {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
        if (sock >= 0) is_dgram_icmp = true;
        if (sock < 0) {
            sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
            if (sock >= 0) is_raw_icmp = true;
        }
        if (sock < 0) {
            sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock >= 0) is_udp_trace = true;
        }
    }

    if (sock < 0) {
        record.success = false;
        record.rtt_ms = -1.0;
        return record;
    }

    int cur_ttl = ttl;
    int recverr = 1;
    if (is_v6) {
        setsockopt(sock, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &cur_ttl, sizeof(cur_ttl));
        setsockopt(sock, IPPROTO_IPV6, IPV6_RECVERR, &recverr, sizeof(recverr));
    } else {
        setsockopt(sock, SOL_IP, IP_TTL, &cur_ttl, sizeof(cur_ttl));
        setsockopt(sock, SOL_IP, IP_RECVERR, &recverr, sizeof(recverr));
    }

    struct sockaddr_storage dest_storage;
    socklen_t dest_len = 0;
    std::memset(&dest_storage, 0, sizeof(dest_storage));

    if (is_v6) {
        struct sockaddr_in6* dest6 = (struct sockaddr_in6*)&dest_storage;
        dest6->sin6_family = AF_INET6;
        inet_pton(AF_INET6, target_ip.c_str(), &dest6->sin6_addr);
        if (is_udp_trace) {
            dest6->sin6_port = htons(static_cast<uint16_t>(33434 + ttl));
        }
        dest_len = sizeof(struct sockaddr_in6);
    } else {
        struct sockaddr_in* dest4 = (struct sockaddr_in*)&dest_storage;
        dest4->sin_family = AF_INET;
        inet_pton(AF_INET, target_ip.c_str(), &dest4->sin_addr);
        if (is_udp_trace) {
            dest4->sin_port = htons(static_cast<uint16_t>(33434 + ttl));
        }
        dest_len = sizeof(struct sockaddr_in);
    }

    int data_len = (payload_size > 0 ? payload_size : 32);
    std::vector<char> send_buf;
    if (is_udp_trace) {
        send_buf.assign(data_len, 'E');
    } else if (is_v6) {
        send_buf.assign(sizeof(struct icmp6_hdr) + data_len, 'E');
        struct icmp6_hdr* icmp6 = (struct icmp6_hdr*)send_buf.data();
        icmp6->icmp6_type = ICMP6_ECHO_REQUEST;
        icmp6->icmp6_code = 0;
        icmp6->icmp6_id = htons(probe_id);
        icmp6->icmp6_seq = htons(probe_seq);
        icmp6->icmp6_cksum = 0;
    } else {
        send_buf.assign(sizeof(struct icmphdr) + data_len, 'E');
        struct icmphdr* icmp = (struct icmphdr*)send_buf.data();
        icmp->type = ICMP_ECHO;
        icmp->code = 0;
        icmp->un.echo.id = htons(probe_id);
        icmp->un.echo.sequence = htons(probe_seq);
        icmp->checksum = 0;
        icmp->checksum = computeChecksum(send_buf.data(), (int)send_buf.size());
    }

    auto t_start = std::chrono::steady_clock::now();
    if (sendto(sock, send_buf.data(), send_buf.size(), 0, (struct sockaddr*)&dest_storage, dest_len) < 0) {
        close(sock);
        return record;
    }

    auto deadline = t_start + std::chrono::milliseconds(timeout_ms);

    while (true) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;

        int remain_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        if (remain_ms <= 0) remain_ms = 1;

        struct pollfd pfd;
        pfd.fd = sock;
        pfd.events = POLLIN | POLLERR;
        pfd.revents = 0;

        int pret = poll(&pfd, 1, remain_ms);
        if (pret <= 0) break;

        auto t_end = std::chrono::steady_clock::now();
        double elapsed_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();

        // 1. 先检查内核扩展错误队列
        std::string err_ip;
        if (readErrorQueueRouterIP(sock, probe_seq, ttl, is_udp_trace, is_v6, err_ip)) {
            record.ip = err_ip;
            record.success = true;
            record.rtt_ms = elapsed_ms;
            close(sock);
            return record;
        }

        // 2. 检查常规接收队列
        if (pfd.revents & POLLIN) {
            char recv_buf[1500];
            struct sockaddr_storage from;
            socklen_t from_len = sizeof(from);
            ssize_t recvd = recvfrom(sock, recv_buf, sizeof(recv_buf), MSG_DONTWAIT, (struct sockaddr*)&from, &from_len);
            if (recvd > 0) {
                if (is_v6) {
                    if (is_dgram_icmp && recvd >= (ssize_t)sizeof(struct icmp6_hdr)) {
                        struct icmp6_hdr* icmp6 = (struct icmp6_hdr*)recv_buf;
                        if (ntohs(icmp6->icmp6_seq) == probe_seq) {
                            char resp_ip[INET6_ADDRSTRLEN];
                            struct sockaddr_in6* from6 = (struct sockaddr_in6*)&from;
                            inet_ntop(AF_INET6, &(from6->sin6_addr), resp_ip, INET6_ADDRSTRLEN);
                            record.ip = resp_ip;
                            record.success = true;
                            record.rtt_ms = elapsed_ms;
                            close(sock);
                            return record;
                        }
                    } else if (is_raw_icmp && recvd >= (ssize_t)sizeof(struct icmp6_hdr)) {
                        struct icmp6_hdr* icmp6 = (struct icmp6_hdr*)recv_buf;
                        if (icmp6->icmp6_type == ICMP6_ECHO_REPLY && ntohs(icmp6->icmp6_seq) == probe_seq) {
                            char resp_ip[INET6_ADDRSTRLEN];
                            struct sockaddr_in6* from6 = (struct sockaddr_in6*)&from;
                            inet_ntop(AF_INET6, &(from6->sin6_addr), resp_ip, INET6_ADDRSTRLEN);
                            record.ip = resp_ip;
                            record.success = true;
                            record.rtt_ms = elapsed_ms;
                            close(sock);
                            return record;
                        }
                    }
                } else {
                    if (is_dgram_icmp && recvd >= (ssize_t)sizeof(struct icmphdr)) {
                        struct icmphdr* icmp_hdr = (struct icmphdr*)recv_buf;
                        if (ntohs(icmp_hdr->un.echo.sequence) == probe_seq) {
                            char resp_ip[INET_ADDRSTRLEN];
                            struct sockaddr_in* from4 = (struct sockaddr_in*)&from;
                            inet_ntop(AF_INET, &(from4->sin_addr), resp_ip, INET_ADDRSTRLEN);
                            record.ip = resp_ip;
                            record.success = true;
                            record.rtt_ms = elapsed_ms;
                            close(sock);
                            return record;
                        }
                    } else if (is_raw_icmp && recvd >= (ssize_t)(sizeof(struct iphdr) + sizeof(struct icmphdr))) {
                        struct iphdr* ip_hdr = (struct iphdr*)recv_buf;
                        int ip_hdr_len = ip_hdr->ihl * 4;
                        if (recvd >= ip_hdr_len + (ssize_t)sizeof(struct icmphdr)) {
                            struct icmphdr* icmp_hdr = (struct icmphdr*)(recv_buf + ip_hdr_len);
                            bool matched = false;

                            if (icmp_hdr->type == ICMP_ECHOREPLY) {
                                if (ntohs(icmp_hdr->un.echo.sequence) == probe_seq) {
                                    matched = true;
                                }
                            } else if (icmp_hdr->type == ICMP_TIME_EXCEEDED) {
                                int inner_offset = ip_hdr_len + sizeof(struct icmphdr);
                                if (recvd >= inner_offset + (ssize_t)sizeof(struct iphdr) + 8) {
                                    struct iphdr* inner_ip = (struct iphdr*)(recv_buf + inner_offset);
                                    int inner_ip_len = inner_ip->ihl * 4;
                                    if (recvd >= inner_offset + inner_ip_len + 8) {
                                        struct icmphdr* inner_icmp = (struct icmphdr*)(recv_buf + inner_offset + inner_ip_len);
                                        if (ntohs(inner_icmp->un.echo.sequence) == probe_seq) {
                                            matched = true;
                                        }
                                    }
                                }
                            }

                            if (matched) {
                                char resp_ip[INET_ADDRSTRLEN];
                                struct sockaddr_in* from4 = (struct sockaddr_in*)&from;
                                inet_ntop(AF_INET, &(from4->sin_addr), resp_ip, INET_ADDRSTRLEN);
                                record.ip = resp_ip;
                                record.success = true;
                                record.rtt_ms = elapsed_ms;
                                close(sock);
                                return record;
                            }
                        }
                    }
                }
            }
        }
    }

    close(sock);
    return record;
}

} // namespace mtr

#endif
