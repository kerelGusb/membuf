sudo cp 99-membuf.rules /etc/udev/rules.d/

sudo udevadm control --reload-rules
sudo udevadm trigger

echo "Rules applied"