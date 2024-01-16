# gpu-passthrough
GPU Passthrough Assistance Scripts

### Acknowledgments:
This script is developed with insights and methodologies adapted from https://gitlab.com/risingprismtv/single-gpu-passthrough.

**Reminder**: The commands and procedures detailed here are tailored for Arch Linux, as demonstrated in my video. For guidelines applicable to other distributions, refer to the aforementioned GitLab link.

---
## Initial Steps:
Ensure your motherboard's BIOS is current for optimal IOMMU grouping.
Verify your system installation in UEFI mode; avoid CSM to prevent Legacy mode, which is incompatible with GPU passthrough.
Intel users: If virtualization settings are limited, update your BIOS and confirm your CPU and motherboard support virtualization.
---
### Bios settings
Depending on your machine CPU, you need to enable certain settings in your BIOS for your passthrough to succeed. Enable the settings listed in this table:

| AMD CPU | Intel CPU |
|---------|-----------|
| IOMMU   | VT-D      |
| NX mode | VT-X      |

Note for Intel : You may not have both options, in that case, just enable the one available to you.
If you do not have any virtualization settings, like said before make sure your BIOS is up to date, and that your CPU and motherboard support virtualization.

---
## Editing GRUB
### Enable IOMMU
Set the parameter respective to your system in the grub config:
| AMD CPU      | Intel CPU     |
|--------------|---------------|
| amd_iommu=on | intel_iommu=on|

Set the parameter ```iommu=pt``` in grub config for safety reasons, regardless of CPU type

Mostly for AMD users, the parameter ```video=efifb:off``` can fix issues when returning back to the host, it is recommended that you add it.

Run ```sudo nvim /etc/default/grub```
Edit the line that starts with ```GRUB_CMDLINE_LINUX_DEFAULT``` so it resembles something like this, **keeping any previous parameters if there are any:**
```
GRUB_CMDLINE_LINUX_DEFAULT="intel_iommu=on iommu=pt"
```
On my clean install with Intel CPU the line looked like this:
```
GRUB_CMDLINE_LINUX_DEFAULT="loglevel=3 quiet intel_iommu=on iommu=pt"
```

**NOTE** If you are having a hard time with switching TTY as requested later on in this document, you will need to add the "```nomodeset```" parameter as well and rebuild GRUB - example:
```
GRUB_CMDLINE_LINUX_DEFAULT="loglevel=3 quiet nomodeset intel_iommu=on iommu=pt"
```

After you edit the file you will need to rebuild GRUB with this command ```sudo grub-mkconfig -o /boot/grub/grub.cfg```

**Dont forget to reboot**

---
## Configuration of libvirt
### Installing the virtualization packages

```
sudo pacman -S virt-manager qemu vde2 ebtables iptables-nft nftables dnsmasq bridge-utils ovmf
```
**Please note:** Conflicts may happen when installing these programs.
A warning like the below example may appear in your terminal:
```:: iptables and iptables-nft are in conflict. Remove iptables? [y/N]```
If you do encounter this kind of message, press ```y``` and ```enter``` to continue the installation.

**Note** - When installing you may see the following:
```
:: There are 3 providers available for qemu:
:: Repository extra
   1) qemu-base  2) qemu-desktop  3) qemu-full
```
Select option 2 by typing ```2``` and then hit ```enter``` to continue.

### Editing the libvirt config files
We now need to edit some of the config files. We will start with libvirt.conf
```
sudo nvim /etc/libvirt/libvrtd.conf
```
Search the file for: ```unix_sock_group = "libvirt"``` (should be around line 85)
Search the file for: ```unix_sock_rw_perms = "0770"``` (should be around line 108).
make sure the both lines are uncommented (remove the ```#```)

Head to the bottom of the file and add the following lines to enable logging:
```
log_filters="3:qemu 1:libvirt"
log_outputs="2:file:/var/log/libvirt/libvirtd.log"
```

### Adding user to the libvirt group
Next you need to add your user to the libvirt group this would give you the correct permissions .
```
sudo usermod -a -G kvm,libvirt $(whoami)
```
To verify the command indeed worked execute the following to see all the groups your user is in:
```
sudo groups $(whoami)
```

### Starting the libvirt service
Execute the 2 commands below to enable and start the service
```
sudo systemctl enable libvirtd
sudo systemctl start libvirtd
```

### Editing the qemu config file
Next we need to edit the qemu.conf
```
sudo nvim /etc/libvirt/qemu.conf
```

Search for and uncomment (remove the ```#```) the following 2 lines (should be around line 519 and 523):
```
user = "root"
group = "root"
```
Now that the lines are active we need to update both to use your username, my login is "rama" so my config would look like this:
```
user = "rama"
group = "rama"
```
Your config file would have your login, the above is just an example.

Next to apply all the changes we need to restart libvirt service
```
sudo systemctl restart libvirtd
```

### Enable the VM default network
This part is optional but it is highly recommended as it prevents a manual step when starting the VM, anything to make life easier :)
Execute the following command:
```
sudo virsh net-autostart default
```

If you opted out from doing the above you will need to run the below command every time before you can start your VM
```
sudo virsh net-start default
```

## Configure the Virtual Machine
We'll focus on setting up an Arch VM. The process is straightforward for current Arch users. I'll also detail Windows 11 installation, but you're free to opt for Windows 10. The beauty of virtualization is in its flexibility!

### Download The virtio tools
head on over to [virtio-win-pkg-scripts](https://github.com/virtio-win/virtio-win-pkg-scripts/blob/master/README.md)
Download the ```Stable virtio-win ISO``` 

Next download the [Arch Linux ISO](https://archlinux.org/download/)

And last, grab the [Windows 11 ISO](https://www.microsoft.com/software-download/windows11)


### Getting the GPU ROM
For proper VM operation with your GPU, accessing the GPU ROM is crucial.
Note for Nvidia GPU users: This step is mandatory. AMD GPU users may skip this.

**Warning!!** Do not download "pre-made" ROMS from the internet, you have been warned.

head on over to to this link and download the [Nvidia NVFlash](https://www.techpowerup.com/download/nvidia-nvflash/) tool. You would want to download the latest **"Linux"** version from the left side.

Once you have the file, extract it and note the location (~/Downloads/ by default).

### Dumping the ROM
Switch to a TTY with ```CTRL + ALT + F3``` (or F4)

Stop your display manager, here are some examples
- SDDM: ```sudo systemctl stop sddm```
- GDM: ```sudo systemctl stop gdm3```

Now we need to unload the drivers so the NVFlash tool can have access to the GPU, execute the following:
```
sudo rmmod nvidia_uvm
sudo rmmod nvidia_drm
sudo rmmod nvidia_modeset
sudo rmmod nvidia
```
next navigate into the NVFlash extracted location and make the utility executable with:
```sudo chmod +x nvflash```

Now run the utility with this command:
```sudo ./nvflash --save vbios.rom```

If everything worked, you should have a new file named **vbios.rom**

Now we need to reload the drivers and get back to the desktop, the easy way is to ```reboot```, but you can also do this:
```
sudo modprobe nvidia
sudo modprobe nvidia_uvm
sudo modprobe nvidia_drm
sudo modprobe nvidia_modeset
```

Now we are ready to start the display manager, so depending which one you stopped earlier you can use:
- SDDM: ```sudo systemctl restart sddm```
- GDM: ```sudo systemctl restart gdm3```

### Patching the ROM
In order to use the ROM it must be first edited with a HEX editor. Download Okteta with:
```
sudo pacman -S okteta
```

Make a copy of the vbios.rom file **keep the original safe** so you have a backup. For the sake of this document we will call the copy "vbios_copy.rom"

Open vbios_copy.rom with Okteta, use ```CTRL + F``` to search the file and change the settings to ```Char``` and search for the value ```VIDEO```.

Now, place your cursor **before the first U** before the ```VIDEO``` value you found and select everything before it. Hit the **INS** (Insert) key to switch Okteta to Edit mode and with everything selected before the **U** hit the "Delete" key on your keyboard.

Save the file as another copy naming it "vbios_patched.rom".

### Moving the ROM
We now need to move the patched ROM to the correct location. First we need to create a directory:
```
sudo mkdir /usr/share/vgabios
```
Now we need to copy the ROM and set the permissions:
```
sudo cp ./vbios_patched.rom /usr/share/vgabios/
cd /usr/share/vgabios
sudo chmod -R 644 vbios_patched.rom
sudo chown yourusername:yourusername vbios_patched.rom
```

## Clone this repo
If you haven't done so already, clone a copy of this repo.
```
git clone https://github.com/0xRama/gpu-passthrough
```
### Execute script
Now cd into the downloaded repo and execute the Install script:
```
cd gpu-pt
sudo ./install_hooks.sh
```

The above script install all the hooks needed to disconnect the video card and allows the GPU to be attached to the VM.

Next execute the "get-group" script with ```./get-group```. Pay attention to the devices in your GPU group as you will later need to attach all of these devices to your VM with the exception of any devices labeled as ```PCI bridge [0604]```

### Validate installed files
You would want to confirm that the following files were created:
```
/etc/systemd/system/libvirt-nosleep@.service
/bin/vfio-startup.sh
/bin/vfio-teardown.sh
/etc/libvirt/hooks/qemu
```

### Update the hook script
We now need to update the hook script so id doesn't execute by mistake as we are building vms:
```
sudo nvim /etc/libvirt/hooks/qemu
```
Edit the line that reads ```if [[ $OBJECT == "win10" ]]; then``` replacing "win10" with "somevm". This is a place holder, we will come back and edit this file again once our VM is ready for the GPU.

## Create a VM
Follow these steps for both Windows and Linux guests, adapting as needed for each VM type using GPU passthrough.


Open "Virtual Machine Manager".
Click "Create new virtual machine".
Step 1: Choose "Local install media (ISO image or CDROM)".
Step 2: "Browse" for your OS ISO.
Step 3: Assign initial resources.
Step 4: Set VM disk storage size.
Step 5: Name your VM uniquely and enable "Customize configuration before install", then "Finish".

### Customize the VM
Modify the VM settings as follows, applying changes before switching tabs:

Overview: Switch "Firmware" to "UEFI".
CPUs: Match vCPU allocation with your system's logical CPU count.
Memory: Allocate half your system's RAM to the VM.
Boot Options: Prioritize "SATA CDROM" as the first boot device.

VirtIO Disk1: In "Advanced options", set "Cache mode" to "writeback".
NIC: Ensure "Device model" is set to "virtio".

- **Windows VM only**: : Use "Add Hardware" to include a "CDROM device" for the "virtio-win..." ISO, essential for the Windows install process.

**Windows 11 Note** To install Windows 11, you'll need a TPM module. First, install the swtpm package:
```
sudo pacman -S swtpm
```
You can then add a TPM from the menu with settings CRB and version 2.0

With all the options set you can now click on "Begin Installation" on the top. This would kick off the OS install process.

### OS Installation Steps

#### For Linux:
The Linux OS installation should proceed smoothly without extra steps. Just follow the standard installation procedure.

#### For Windows:
Installing Windows requires additional steps to incorporate the virtio drivers:

1. During Windows Setup, at the "Where do you want to install Windows?" prompt, the list will be empty due to VIRTIO.
2. Click "Load driver."
3. Insert the drive containing the virtio-win ISO.
4. Navigate to "amd64", then to your specific Windows version folder (e.g., w11).
5. The virtio driver should now be visible in the list. Select it and click "Next."
6. The VM drive will appear, allowing you to select it as the installation target.

After Windows installation and booting to the desktop:

- Access the virtio-win ISO through File Explorer.
- Locate and run "virtio-win-gt-x64" within the drive.
- Follow the on-screen prompts, accepting the default settings. This will ensure all devices function correctly.

#### Internet Connection Check:
Verify that your VM has internet access. Then power down the OS to adjust the VM configuration for GPU integration.

## Post-OS Installation VM Configuration
With the VM powered off, remove these items from the configuration:

- Any "USB Redirector."
- Display Spice.
- Video QXL.

This step is crucial for optimizing the VM's performance and ensuring that it utilizes the GPU passthrough effectively.

If you are unable to remove all of the above via the GUI then you will need to edit the XML and remove the following:
```
  <graphics type="spice" autoport="yes">
    <listen type="address"/>
    <gl enable="no"/>
  </graphics>
```
```
  <audio id="1" type="none"/>
```
```
  <video>
    <model type="bochs" vram="16384" heads="1" primary="yes"/>
    <address type="pci" domain="0x0000" bus="0x05" slot="0x00" function="0x0"/>
  </video>
```
```
  <channel type="spicevmc">
    <target type="virtio" name="com.redhat.spice.0"/>
    <address type="virtio-serial" controller="0" bus="0" port="1"/>
  </channel>
```

### GPU Integration
Now it's time to integrate the GPU into the VM:

1. Click "Add Hardware."
2. Choose "PCI Host Device."
3. Add each component identified in your GPU group.

**For Nvidia GPUs**:

- Select the primary GPU PCI device.
- Access the XML configuration.
- Below the "`</source>`" tag, insert a new line: `<rom file='/usr/share/vgabios/vbios_patched.rom'/>`.
- Click "Apply."

### USB Device Passthrough

To pass USB devices (like keyboards, mice, headphones, game controllers, etc.) to your VM, follow these steps:

1. Click on "Add Hardware."
2. Choose "USB Host Device."
3. Select the desired USB device from the list.

**Important**: Do not boot the VM yet. This step ensures that all the necessary hardware components are correctly integrated before starting up the VM.


## Setup VM hook
To effectively switch your GPU between the host and the VM, set up a VM hook that activates scripts for disconnecting the GPU from the host and connecting it to the VM.
```
sudo nvim /etc/libvirt/hooks/qemu
```
Edit the line that reads ```if [[ $OBJECT == "somevm" ]]; then``` replacing "somevm" with the name of your new VM. If you have multiples you can add them in with the ```||``` (or operator), for example:
```
if [[ $OBJECT == "vm1" || $OBJECT == "vm2" ]]; then
```

## boot the VM
Now you're ready to boot your VM. For Linux, it should work seamlessly right away. In the case of Windows, initially, you'll encounter a black screen. However, after a brief period, Windows will automatically connect to the internet, download, and install the Nvidia driver. Once this process is complete, the desktop will appear.

Congratulations!!