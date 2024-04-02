#!/bin/bash
# This file is the install instruction for the CHROOT build
# We're using cloudsmith-cli to upload the file in CHROOT
sudo apt install -y python3-pip
sudo pip3 install --upgrade cloudsmith-cli
curl -1sLf 'https://dl.cloudsmith.io/public/openhd/release/setup.deb.sh'| sudo -E bash
apt update
sudo apt install -y git ruby-dev curl make cmake gcc g++ wget libdrm-dev libcairo-dev
git clone https://github.com/rockchip-linux/mpp.git
sudo cmake --build build --target install
gem install fpm
cmake -B build
sudo cmake --build build --target install


VERSION="1.2-$(date -d '+1 hour' +'%m-%d-%Y--%H-%M-%S')"
VERSION=$(echo "$VERSION" | sed 's/\//-/g')
# fpm -a arm64 -s dir -t deb -n mpp-rk3566 -v "$VERSION" -C mpp-package -p mpp-rk3566_VERSION_ARCH.deb
echo "push to cloudsmith"
git describe --exact-match HEAD >/dev/null 2>&1
echo "Pushing the package to OpenHD 2.3 repository"
API_KEY=$(cat /opt/additionalFiles/cloudsmith_api_key.txt)
DISTRO=$(cat /opt/additionalFiles/distro.txt)
FLAVOR=$(cat /opt/additionalFiles/flavor.txt)
BOARD=$(cat /opt/additionalFiles/board.txt)

if [ "$BOARD" = "rk3588" ]; then
    for file in *.deb; do
        mv "$file" "${file%.deb}-rk3588.deb"
    done
    cloudsmith push deb --api-key "$API_KEY" openhd/dev-release/${DISTRO}/${FLAVOR} *.deb || exit 1
else
for file in *.deb; do
        mv "$file" "${file%.deb}-rk3566.deb"
    done
    cloudsmith push deb --api-key "$API_KEY" openhd/dev-release/${DISTRO}/${FLAVOR} *.deb || exit 1
fi
