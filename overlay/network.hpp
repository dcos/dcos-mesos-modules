#include <stdio.h>

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

class IPWrapper
{
public:
  //default constructor
  IPWrapper()
    : address_(net::IP(0)) {}

  // paramatized constructor
  IPWrapper(const uint32_t _address) 
    : address_(net::IP(_address)) {}

  IPWrapper(const struct in6_addr& _storage)
    : address_(net::IP(_storage)) {}

  bool operator==(const IPWrapper& that) const
  {
    return address_ == that.address();
  }

  bool operator!=(const IPWrapper& that) const
  {
    return address_ != that.address();
  }

  bool operator<(const IPWrapper& that) const
  {
    return address_ < that.address();
  }

  bool operator>(const IPWrapper& that) const
  {
    return address_ > that.address();
  } 

  IPWrapper& operator++() {
    switch (address_.family()) {
      case AF_INET: {
       uint32_t address = ntohl(address_.in().get().s_addr);
       address_ = net::IP(address + 1);
       break;
      }
      case AF_INET6: {
        in6_addr addr6 = address_.in6().get();
        for (int i = 15; i >= 0; i--) {
          if (addr6.s6_addr[i] != 0xff) {
            addr6.s6_addr[i]++;
            index_ = i;
            break;
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

  IPWrapper& operator--() {
    switch (address_.family()) {
      case AF_INET: {
        uint32_t address = ntohl(address_.in().get().s_addr);
        address_ = net::IP(address - 1);
        break;
      }
      case AF_INET6: {
        in6_addr addr6 = address_.in6().get();
        addr6.s6_addr[index_]--;
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

  net::IP address() const { return address_; }

private:
  net::IP address_;
  uint8_t index_;
};

inline std::ostream& operator<<(std::ostream& stream, const IPWrapper& _ip)
{
  stream << _ip.address();
  return stream;
}

class Network
{
public:
  // default constructor
  Network() 
    :network_(net::IPNetwork::create(IP(0), IP(0)).get()),
     prefix_(0)
  {}  

  // paramatized constructor
  Network(const net::IP& _address, uint8_t _prefix)
    :network_(net::IPNetwork::create(_address, _prefix).get()),
     prefix_(_prefix)
  {}

  bool operator==(const Network& that) const
  {
    return network_ == that.network();
  }

  bool operator!=(const Network& that) const
  {
    return network_ != that.network();
  }

  bool operator<(const Network& that) const
  {
    if (prefix_ != that.prefix()) {
      return prefix_ > that.prefix();
    } else {
      return network_.address() < that.network().address();
    }
  }

  bool operator>(const Network& that) const
  {
    if (prefix_ != that.prefix()) {
      return prefix_ < that.prefix();
    } else {
      return network_.address() > that.network().address();
    }
  }

  Network& operator++() {
    net::IP address = network_.address();
    switch (address.family()) {
      case AF_INET: {
        uint32_t address_ = ntohl(address.in().get().s_addr);
        address_ = address_ >> (32 - prefix_);
        address_ = (address_ + 1) << (32 - prefix_);
        network_ = net::IPNetwork::create(IP(address_), prefix_).get();
        break;
      }
      case AF_INET6: {
        bool incremented = false;
        uint8_t startInx = prefix_ >> 3;
        in6_addr addr6 = address.in6().get();

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

        network_ = net::IPNetwork::create(IP(addr6), prefix_).get();  
        break;
      }
      default: {
        UNREACHABLE();
      }
   }
   return *this;
  }

  Network& operator--() {
    net::IP address = network_.address();
    switch (address.family()) {
      case AF_INET: {
        uint32_t address_ = ntohl(address.in().get().s_addr);
        address_ = address_ >> (32 - prefix_);
        address_ = (address_ - 1) << (32 - prefix_);
        network_ = net::IPNetwork::create(IP(address_), prefix_).get();
        break;
      }
      case AF_INET6: {
        in6_addr addr6 = address.in6().get();
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

        network_ = net::IPNetwork::create(IP(addr6), prefix_).get();
        break;
      }
      default: {
        UNREACHABLE();
      }
    }
    return *this;
  }

  net::IPNetwork network() const { return network_; }
  uint8_t prefix() const { return prefix_; }

private:
  net::IPNetwork network_;
  uint8_t prefix_;
  uint8_t index_;
};

//Returns the string representation of the given IP network using the
//canonical form with prefix. For example: "10.0.0.1/8".
inline std::ostream& operator<<(std::ostream& stream, const Network& _network)
{
  stream << _network.network().address() << "/" << _network.network().prefix();
  return stream;
}

} // overlay
} // modules
} // mesos 
