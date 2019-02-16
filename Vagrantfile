# -*- mode: ruby -*-
# vi: set ft=ruby :

Vagrant.configure(2) do |config|
  config.vm.box = "ubuntu/bionic64"

  config.vm.synced_folder "..", "/work", type: "virtualbox"

  config.vm.provider "virtualbox" do |v|
    v.linked_clone = true
    v.memory = 8192
    v.cpus = 4
  end

  config.vm.provision :shell, inline: <<-SHELL
    sudo apt-get update

    sudo apt-get install -y \
        clang lld \
        clang-format clang-tidy clang-tools \
        tar wget git \
        openjdk-8-jdk \
        autoconf libtool \
        build-essential python-dev python-six \
        python-virtualenv libcurl4-nss-dev \
        libsasl2-dev libsasl2-modules maven \
        libapr1-dev libsvn-dev zlib1g-dev iputils-ping \
        libevent-dev libssl-dev libsystemd-dev pkgconf \
        iptables ipset
    sudo update-alternatives --set cc /usr/bin/clang
    sudo update-alternatives --set c++ /usr/bin/clang++

    curl -fsSL https://get.docker.com/ | sudo sh
    sudo usermod -aG docker vagrant
  SHELL
end
