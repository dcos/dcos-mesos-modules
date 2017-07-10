#ifndef __OVERLAY_NETWORK_HPP__
#define __OVERLAY_NETWORK_HPP__

#include <stdio.h>

#include <boost/functional/hash.hpp>

#include <stout/check.hpp>
#include <stout/interval.hpp>
#include <stout/ip.hpp>
#include <stout/json.hpp>
#include <stout/mac.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

using std::hex;
using net::IP;
using net::IPNetwork;
using net::MAC;

namespace mesos {
namespace modules {
namespace overlay {

class IP : public net::IP
{
public:
  //default constructor
  IP()
    : net::IP(0) {}

  // paramatized constructor
  IP(const uint32_t _address) 
    : net::IP(_address) {}

  IP(const struct in_addr& _storage)
    : net::IP(_storage) {}

  IP(const struct in6_addr& _storage)
    : net::IP(_storage) {}

  static Try<IP> convert(const net::IP& ip);
  static Try<IP> parse(const std::string& value, int family);

  bool operator==(const IP& that) const
  {
    return net::IP::operator==(that);
  }

  bool operator!=(const IP& that) const
  {
    return net::IP::operator!=(that);
  }

  bool operator<(const IP& that) const
  {
    return net::IP::operator<(that);
  }

  bool operator>(const IP& that) const
  {
    return net::IP::operator>(that);
  } 

  IP& operator++() {
    switch (family_) {
      case AF_INET: {
       uint32_t address = ntohl(storage_.in_.s_addr);
       storage_.in_.s_addr = htonl(address + 1);
       break;
      }
      case AF_INET6: {
        in6_addr addr6 = storage_.in6_;
        for (int i = 15; i >= 0; i--) {
          if (addr6.s6_addr[i] != 0xff) {
            addr6.s6_addr[i]++;
            index_ = i;
            break;
          }
        } 
        storage_.in6_  = addr6;
        break;
      } 
      default: {
       UNREACHABLE();
      }
    }
    return *this;
  }

  IP& operator--() {
    switch (family_) {
      case AF_INET: {
        uint32_t address = ntohl(storage_.in_.s_addr);
        storage_.in_.s_addr = htonl(address - 1);
        break;
      }
      case AF_INET6: {
        in6_addr addr6 = storage_.in6_;
        addr6.s6_addr[index_]--;
        if (!addr6.s6_addr[index_]) {
          index_--;
        }
        storage_.in6_ = addr6;
        break;
      }
      default: {
        UNREACHABLE();
      }
    }
    return *this;
  } 

private:
  uint8_t index_;
};

inline Try<IP> IP::convert(const net::IP& ip)
{
  switch(ip.family()) {
    case AF_INET:
      return IP(ip.in().get());
    case AF_INET6:
      return IP(ip.in6().get());
    default:
      UNREACHABLE();
  }
}

inline Try<IP> IP::parse(const std::string& value, int family)
{
  Try<net::IP> ip = net::IP::parse(value, family);
  if (ip.isError()) {
    return Error(ip.error());
  }

  switch(family) {
    case AF_INET:
      return IP(ip.get().in().get());
    case AF_INET6:
      return IP(ip.get().in6().get());
    default:
      UNREACHABLE();
  }
}
  

// Returns the string representation of the given IP using the
// canonical form, for example: "10.0.0.1" or "fe80::1".
inline std::ostream& operator<<(std::ostream& stream, const IP& ip)
{
  stream << ip;
  return stream;
}

class Network : public net::IPNetwork
{
public:
  // default constructor
  Network() 
    :net::IPNetwork(net::IP(0), net::IP(0)),
     prefix_(0)
  {}  

  // paramatized constructor
  Network(const net::IP& address, uint8_t prefix)
    :net::IPNetwork(address, toMask(prefix, address.family())),
     prefix_(prefix)
  {}

  // Creates an IP network from the given IP address and netmask.
  // Returns error if the netmask is not valid (e.g., not contiguous).
  static Try<Network> parse(const std::string& value, int family = AF_UNSPEC);

  // Helper function to convert prefix to netmask
  static net::IP toMask(uint8_t prefix, int family); 

  bool operator==(const Network& that) const
  {
    return net::IPNetwork::operator==(that);
  }

  bool operator!=(const Network& that) const
  {
    return net::IPNetwork::operator!=(that);
  }

  bool operator<(const Network& that) const
  {
    if (prefix_ != that.prefix()) {
      return prefix_ > that.prefix();
    } else {
      return address() < that.address();
    }
  }

  bool operator>(const Network& that) const
  {
    if (prefix_ != that.prefix()) {
      return prefix_ < that.prefix();
    } else {
      return address() > that.address();
    }
  }

  Network& operator++() {
    switch (address_.family()) {
      case AF_INET: {
        uint32_t addr = ntohl(address_.in().get().s_addr);
        addr = addr >> (32 - prefix_);
        addr = (addr + 1) << (32 - prefix_);
        address_ = net::IP(addr);
        break;
      }
      case AF_INET6: {
        bool incremented = false;
        uint8_t startInx = prefix_ >> 3;
        in6_addr addr6 = address_.in6().get();

        if (prefix_ & 7) {
          uint8_t bitshift = 8 - (prefix_ & 7);
          uint8_t lowerbits = addr6.s6_addr[startInx] >> bitshift;
          if ((lowerbits & 7) != 7) {
            addr6.s6_addr[startInx] = (lowerbits + 1) << bitshift;
            index_ = startInx;
            incremented = true;
          }
        }

        if (!incremented) {
          for (int i = startInx - 1; i >= 0; i--) {
            if (addr6.s6_addr[i] != 0xff) {
              addr6.s6_addr[i]++;
              index_ = i;
              break;
            }   
          }
        }       

        address_ = net::IP(addr6);  
        break;
      }
      default: {
        UNREACHABLE();
      }
   }
   return *this;
  }

  Network& operator--() {
    switch (address_.family()) {
      case AF_INET: {
        uint32_t addr = ntohl(address_.in().get().s_addr);
        addr = addr >> (32 - prefix_);
        addr = (addr - 1) << (32 - prefix_);
        address_ = net::IP(addr);
        break;
      }
      case AF_INET6: {
        in6_addr addr6 = address_.in6().get();
        if ((prefix_ & 7) && (index_ == prefix_ >> 3)) {
          uint8_t bitshift = 8 - (prefix_ & 7);
          uint8_t lowerbits = addr6.s6_addr[index_] >> bitshift;
          addr6.s6_addr[index_] = (lowerbits - 1) << bitshift;
        } else {
          addr6.s6_addr[index_]--;
        }

        if (!addr6.s6_addr[index_]) {
          index_--;
        }

        address_ = net::IP(addr6);
        break;
      }
      default: {
        UNREACHABLE();
      }
    }
    return *this;
  }

  uint8_t prefix() const { return prefix_; }

private:
  uint8_t prefix_;
  uint8_t index_;
};

inline Try<Network> Network::parse(const std::string& value, int family)
{
  Try<net::IPNetwork> ipNetwork = net::IPNetwork::parse(value, family);
  if (ipNetwork.isError()) {
    return Error(ipNetwork.error());
  }

  return Network(ipNetwork.get().address(), ipNetwork.get().prefix());
}
  
inline net::IP Network::toMask(uint8_t prefix, int family)
{
  switch (family) {
    case AF_INET: {
      uint32_t mask = 0xffffff << (32 - prefix);
      return net::IP(mask); 
    }
    case AF_INET6: {
      in6_addr mask;
      memset(&mask, 0, sizeof(mask));

      int i = 0;
      while (prefix >= 8) {
        mask.s6_addr[i++] = 0xff;
        prefix -= 8;
      }

      if (prefix > 0) {
        uint8_t _mask = 0xff << (8 - prefix);
        mask.s6_addr[i] = _mask;
      }
      return net::IP(mask); 
    }
    default: 
      UNREACHABLE();
  }
}

//Returns the string representation of the given IP network using the
//canonical form with prefix. For example: "10.0.0.1/8".
inline std::ostream& operator<<(std::ostream& stream, const Network& _network)
{
  stream << _network;
  return stream;
}

} // overlay
} // modules
} // mesos 


namespace std {

template <>
struct hash<mesos::modules::overlay::IP>
{
  typedef size_t result_type;

  typedef mesos::modules::overlay::IP argument_type;

  result_type operator()(const argument_type& ip) const
  {
    size_t seed = 0;

    switch (ip.family()) {
      case AF_INET:
        boost::hash_combine(seed, htonl(ip.in().get().s_addr));
        return seed;
      case AF_INET6: {
        in6_addr in6 = ip.in6().get();
        boost::hash_range(seed, std::begin(in6.s6_addr), std::end(in6.s6_addr));
        return seed;
      }
      default:
        UNREACHABLE();
    }
  }
};

} // namespace std

#endif // __OVERLAY_NETWORK_HPP__
