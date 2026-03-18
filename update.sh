make
sudo mkdir -p /usr/local/bin/
sudo mv dvorak /usr/local/bin/dvorak && sudo chown root /usr/local/bin/dvorak
sudo cp ./examples/dvorak-signal.sh /usr/local/bin/dvorak-signal.sh && sudo chown root /usr/local/bin/dvorak-signal.sh && sudo chmod +x /usr/local/bin/dvorak-signal.sh
sudo cp ./examples/dvorak-start.sh /usr/local/bin/dvorak-start.sh && sudo chown root /usr/local/bin/dvorak-start.sh && sudo chmod +x /usr/local/bin/dvorak-start.sh
cp ./examples/sway_layout-watcher.sh ~/.config/sway/scripts/layout-watcher.sh
sudo systemctl restart dvorak-usb dvorak-bt
