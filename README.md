# Master GPU Passthrough Guide (QEMU/KVM)

This guide provides a comprehensive walkthrough for setting up GPU passthrough to a virtual machine (VM) using QEMU/KVM. It aims to consolidate information for various scenarios, including desktops with traditional (muxed) GPUs and laptops with muxless dedicated GPUs (dGPUs).

**Disclaimer:** GPU passthrough can be complex and varies significantly based on hardware and software. While this guide strives for accuracy and completeness, proceed with caution and ensure you understand the steps before making changes to your system. Always back up important data.

## Introduction

### What is GPU Passthrough?

GPU passthrough (also known as VFIO, Virtual Function I/O) allows a virtual machine to have direct and exclusive control over a physical GPU installed in the host system. This enables near-native graphics performance within the VM, making it suitable for demanding tasks like gaming, CAD, video editing, and GPU-accelerated computing.

### Benefits

*   **Near-native graphics performance** in the VM.
*   Run GPU-intensive Windows games or applications on a Linux host.
*   Isolate graphics drivers and workloads within a VM.
*   Utilize specific GPU features or software only available on certain operating systems.

### Overview of This Guide

This guide is structured into several phases:

1.  **Host System Preparation:** Configuring your BIOS and host Linux operating system.
2.  **VBIOS Handling:** Obtaining and preparing your GPU's VBIOS (Video BIOS).
3.  **Libvirt Hook Scripts:** Automating the process of detaching the GPU from the host and attaching it to the VM.
4.  **Virtual Machine Creation & Configuration:** Setting up the VM itself.
5.  **GPU Passthrough & Final VM Tweaks:** Assigning the GPU to the VM and making final adjustments.
6.  **Booting and Troubleshooting:** Launching your VM and addressing common issues.
7.  **USB and Other Device Passthrough:** Passing through USB controllers, individual USB devices, evdev for input devices, and briefly mentioning general PCI device passthrough.
8.  **Performance Optimizations:** CPU pinning, HugePages, and other tweaks for optimal performance.
9.  **Security Considerations:** Discussing potential security risks and best practices.

### General Prerequisites

*   **CPU with Virtualization Support:** Intel VT-x and VT-d, or AMD-V (SVM) and IOMMU.
*   **Motherboard with Virtualization Support:** Corresponding BIOS options for VT-d/IOMMU.
*   **UEFI Boot:** The host system should be installed and booting in UEFI mode. CSM/Legacy boot is generally incompatible.
*   **Sufficient RAM:** Enough RAM for both the host OS and the VM (e.g., 16GB+ recommended).
*   **Adequate Storage:** Enough disk space for the host OS, VM virtual disk, and any applications/games.
*   **Two GPUs (Recommended for Desktops/Muxed):** One for the host and one dedicated to the VM. Laptops with muxless dGPUs can often work with a single dGPU being passed through, relying on an iGPU for the host or using solutions like Looking Glass.

### Important Note on GPU Types

Understanding your GPU setup is crucial:

*   **Muxed GPUs:** Typically found in desktop systems where the dedicated GPU's output is directly connected to monitor ports. Some older laptops also use this setup. In `lspci -nnk` output, the passthrough candidate GPU might be listed as a `VGA compatible controller` and often has its own IOMMU group cleanly separated from other essential devices.
*   **Muxless dGPUs:** Common in modern gaming laptops. The dedicated GPU (e.g., Nvidia) renders frames, but its output is often routed through the integrated GPU (iGPU, e.g., Intel) to the laptop's internal display. The dGPU might appear as a `3D controller` in `lspci -nnk` output. Passthrough for these setups often requires more intricate VBIOS handling, OVMF patching, and solutions like Looking Glass for display output if the internal display is driven by the iGPU after passthrough.

**How to Check:**
Run `lspci -nnk` in your host terminal. Identify your dedicated GPU. If it's listed primarily as a `VGA compatible controller`, you likely have a muxed setup. If it's a `3D controller` and you have an iGPU also listed as `VGA compatible controller`, you likely have a muxless setup.

This guide will provide specific instructions or considerations for both scenarios where their paths diverge.

---

## Phase 1: Host System Preparation

This phase involves configuring your computer's BIOS/UEFI and your host Linux operating system to support IOMMU and prepare for virtualization.

### 1.1 BIOS/UEFI Configuration

Access your system's BIOS/UEFI setup utility (usually by pressing Del, F2, F10, or Esc during boot). The exact names and locations of these settings vary by manufacturer and motherboard model.

*   **Enable Virtualization Technology:**
    *   Intel: Enable **Intel Virtualization Technology (VT-x)** and **Intel VT-d** (or similar, e.g., Directed I/O).
    *   AMD: Enable **SVM (Secure Virtual Machine)** mode and **IOMMU** (or similar, e.g., AMD-Vi).
*   **Ensure UEFI Boot:** Confirm that your system is set to boot in **UEFI mode**. Disable **CSM (Compatibility Support Module)** or **Legacy Boot** options if present.
*   **Disable Secure Boot:** Secure Boot can sometimes interfere with custom kernel modules or VFIO. It's often recommended to disable it, especially if you plan on patching OVMF or using unsigned drivers. This is particularly relevant for the Muxless laptop setup described later.
*   **(Optional) Primary Display:** If you have multiple GPUs and your BIOS allows, set the GPU intended for the host as the primary display device.

Save changes and exit the BIOS/UEFI setup.

### 1.2 Enable IOMMU in Bootloader

The IOMMU (Input/Output Memory Management Unit) is essential for isolating PCI devices for passthrough. You need to enable it via kernel parameters in your bootloader.

**Common Kernel Parameters:**

*   For Intel CPUs: `intel_iommu=on`
*   For AMD CPUs: `amd_iommu=on`
*   For both (recommended for passthrough mode): `iommu=pt` (This passes through IOMMU translations to the hardware, potentially improving performance and compatibility, but might hide some devices from the host if they are not bound to a driver.)
*   Optional (may help with host display issues after VM shutdown, especially on AMD): `video=efifb:off`
*   Optional (if you experience issues switching to a TTY, or host graphics driver issues): `nomodeset` (Add this temporarily for troubleshooting; it disables kernel mode setting).

**Editing GRUB (Most Common Bootloader):**

1.  Edit the GRUB configuration file:
    ```bash
    sudo nvim /etc/default/grub # Or your preferred text editor (e.g., nano, gedit)
    ```
2.  Locate the line starting with `GRUB_CMDLINE_LINUX_DEFAULT`.
3.  Add the required parameters. For example, on an Intel system:
    ```
    GRUB_CMDLINE_LINUX_DEFAULT="quiet splash intel_iommu=on iommu=pt"
    ```
    (Keep any existing parameters like `quiet` or `loglevel`.)
4.  Save the file and update your GRUB configuration:
    ```bash
    sudo grub-mkconfig -o /boot/grub/grub.cfg
    ```
    (The output path might vary on some distributions, e.g., `/boot/efi/EFI/distroname/grub.cfg` for EFI systems).

**For other bootloaders (e.g., systemd-boot):** Consult your distribution's documentation for adding kernel parameters.

**Reboot your system** for these changes to take effect.

**Verify IOMMU is Active:**
After rebooting, you can check if IOMMU is enabled by looking for IOMMU messages in your kernel log:
```bash
dmesg | grep -i -e DMAR -e IOMMU
```
You should see output indicating IOMMU (or DMAR for Intel) remapping is active.

You can also check IOMMU groups using the `get-group` script provided in this repository later, or manually by listing devices in `/sys/kernel/iommu_groups/`.

### 1.3 Install Virtualization Packages

You'll need QEMU, KVM, libvirt, and other utilities. The package names can vary by distribution.

**For Arch Linux / Arch-based distributions (as per `README.md`):**
```bash
sudo pacman -S virt-manager qemu vde2 ebtables iptables-nft nftables dnsmasq bridge-utils ovmf edk2-ovmf
```
*   **Note on Conflicts:** If prompted about conflicts (e.g., `iptables` and `iptables-nft`), it's usually safe to choose the `-nft` version (e.g., remove `iptables` by typing `y`).
*   **QEMU Provider:** You might be asked to choose a QEMU provider (e.g., `qemu-desktop`, `qemu-full`). `qemu-desktop` is generally sufficient.

**For NixOS (as per `README-Muxless.md` principles):**
Configuration is typically done declaratively in `configuration.nix`. You would enable libvirt and add necessary packages to `environment.systemPackages`.
```nix
{
  virtualisation.libvirtd.enable = true;
  programs.virt-manager.enable = true;
  environment.systemPackages = with pkgs; [
    qemu
    # ... other necessary packages like edk2, virtiofsd
  ];
  # Ensure user is in libvirtd group
  users.users.yourusername.extraGroups = [ "libvirtd" ]; 
}
```

**For Debian/Ubuntu-based distributions:**
```bash
sudo apt update
sudo apt install qemu-kvm libvirt-daemon-system libvirt-clients bridge-utils virt-manager ovmf 
```

`ovmf` or `edk2-ovmf` provides UEFI firmware for VMs.

### 1.4 Configure Libvirt

Libvirt is a toolkit to manage virtualization platforms.

1.  **Edit `libvirtd.conf`:**
    ```bash
    sudo nvim /etc/libvirt/libvirtd.conf
    ```
    Uncomment (remove the `#` at the beginning) and ensure the following lines are set:
    ```
    unix_sock_group = "libvirt"
    unix_sock_rw_perms = "0770"
    ```
    For enhanced logging (optional but helpful for troubleshooting), add or uncomment near the end:
    ```
    log_filters="3:qemu 1:libvirt"
    log_outputs="2:file:/var/log/libvirt/libvirtd.log"
    ```
2.  **Add your user to the `libvirt` and `kvm` groups:**
    ```bash
    sudo usermod -aG libvirt $(whoami)
    sudo usermod -aG kvm $(whoami) # 'kvm' group might not be necessary on all distros if libvirt permissions are correctly set
    ```
    You'll need to log out and log back in, or reboot, for group changes to take full effect. Verify with `groups $(whoami)`.
3.  **Enable and start the `libvirtd` service:**
    ```bash
    sudo systemctl enable libvirtd.service
    sudo systemctl start libvirtd.service
    ```
4.  **Edit `qemu.conf`:**
    ```bash
    sudo nvim /etc/libvirt/qemu.conf
    ```
    The original `README.md` suggests changing `user` and `group` to your username. However, it's common and often recommended for security and proper operation (especially with system-wide services) to run QEMU processes as `root` when managed by the system libvirt daemon, or a dedicated QEMU user. For system-level libvirt, ensure the following lines are set (they are often the default if commented out):
    ```
    user = "root"
    group = "root"
    ```
    If you prefer to run as your user (as in `README.md`), ensure your user has appropriate permissions for all resources QEMU might need. This guide will assume `user = "root"`, `group = "root"` for system libvirt for broader compatibility unless issues arise. If you changed this to your user as per the original `README.md`:
    ```
    # user = "yourusername"
    # group = "yourusername"
    ```
    And **ensure your user owns necessary files like the VBIOS ROM if referenced directly by QEMU under user context.**

5.  **Restart `libvirtd` service to apply changes:**
    ```bash
    sudo systemctl restart libvirtd.service
    ```
6.  **Autostart default libvirt network (optional but recommended):**
    ```bash
    sudo virsh net-autostart default
    sudo virsh net-start default # To start it immediately if not already running
    ```
    If you don't autostart, you might need to run `sudo virsh net-start default` before starting VMs that use it.

### 1.5 Isolate the GPU from Host (Early Binding to vfio-pci)

To pass through a device, the host OS must not be using it. The `vfio-pci` kernel module is designed to claim devices for passthrough purposes. We need to tell the system to bind the target GPU (and its associated devices like audio controller) to `vfio-pci` early in the boot process.

1.  **Identify GPU and Associated Devices:**
    Use `lspci -nnk` to find your GPU(s). Note the PCI bus IDs (e.g., `01:00.0`) and the Vendor:Device ID pairs (e.g., `[10de:1c94]`).

    Example output for an Nvidia GPU:
    ```
    01:00.0 VGA compatible controller [0300]: NVIDIA Corporation GP107M [GeForce MX350] [10de:1c94] (rev a1)
            Subsystem: Lenovo Device [17aa:3f9b]
            Kernel driver in use: nvidia
            Kernel modules: nvidiafb, nouveau, nvidia_drm, nvidia
    01:00.1 Audio device [0403]: NVIDIA Corporation GP107GL High Definition Audio Controller [10de:0fb9] (rev a1)
            Subsystem: Lenovo Device [17aa:3f9b]
            Kernel driver in use: snd_hda_intel
            Kernel modules: snd_hda_intel
    ```
    In this example, you'd want to pass through both `01:00.0` (Video) and `01:00.1` (Audio). Their IDs are `10de:1c94` and `10de:0fb9`.

    The `get-group` script in this repository can help identify all devices within the same IOMMU group as your GPU. Generally, all devices in the target GPU's IOMMU group (except PCI bridges) should be passed through together.

2.  **Configure `vfio-pci` to Claim Devices:**
    There are several ways to do this. Using kernel parameters is often the simplest if it works for your setup.

    *   **Method 1: Kernel Parameters (Recommended for simplicity):**
        Add `vfio-pci.ids=xxxx:xxxx,yyyy:yyyy` to your `GRUB_CMDLINE_LINUX_DEFAULT` in `/etc/default/grub`, replacing `xxxx:xxxx` and `yyyy:yyyy` with the Vendor:Device IDs of your GPU and its associated functions.
        Example for the Nvidia GPU above:
        `GRUB_CMDLINE_LINUX_DEFAULT="quiet splash intel_iommu=on iommu=pt vfio-pci.ids=10de:1c94,10de:0fb9"`
        Then update GRUB and reboot: `sudo grub-mkconfig -o /boot/grub/grub.cfg && sudo reboot`

    *   **Method 2: Modprobe Options:**
        Create a `.conf` file in `/etc/modprobe.d/` (e.g., `vfio.conf`):
        ```bash
        sudo nvim /etc/modprobe.d/vfio.conf
        ```
        Add the line:
        `options vfio-pci ids=10de:1c94,10de:0fb9`
        You'll also need to ensure `vfio-pci` is loaded early. Add `vfio_pci`, `vfio`, `vfio_iommu_type1` to your initramfs modules. The exact method varies by distribution (e.g., editing `/etc/mkinitcpio.conf` on Arch and running `sudo mkinitcpio -P`, or `/etc/initramfs-tools/modules` on Debian/Ubuntu and running `sudo update-initramfs -u -k all`).
        This method is more robust if kernel parameters alone don't work reliably.

    *   **NixOS Configuration Example (from `README-Muxless.md`):**
        In your `configuration.nix`:
        ```nix
        boot.kernelParams = [ "intel_iommu=on" "iommu=pt" "vfio-pci.ids=10de:1c94,10de:0fb9" ]; # Add your GPU IDs
        boot.initrd.kernelModules = [ "vfio_pci" "vfio" "vfio_iommu_type1" ];
        # OR sometimes, early loading via modprobe is needed
        # boot.extraModprobeConfig = ''
        #   options vfio-pci ids=10de:1c94,10de:0fb9 disable_vga=1
        # '';
        ```

3.  **Reboot** after making these changes.

4.  **Verify `vfio-pci` is in use:**
    After rebooting, run `lspci -nnk` again. For the GPU and its audio device, you should see:
    ```
    Kernel driver in use: vfio-pci
    Kernel modules: nvidiafb, nouveau, nvidia_drm, nvidia, vfio-pci 
    ```
    The important part is `Kernel driver in use: vfio-pci`.

This completes the initial host system preparation. The next phase will cover obtaining and preparing your GPU's VBIOS.

---

## Phase 2: VBIOS Handling

The VBIOS (Video BIOS) is firmware on your graphics card that initializes it and provides basic display functions before the operating system's drivers load. For GPU passthrough, providing a clean VBIOS to the VM can be essential, particularly for:

*   **Nvidia GPUs:** Often require a VBIOS to initialize correctly in the VM, avoiding issues like Code 43 errors in Windows.
*   **Muxless Laptops:** May require a VBIOS specifically extracted from the system's firmware and potentially patched into the VM's UEFI (OVMF) for the dGPU to function correctly.
*   **Ensuring proper initialization** and avoiding conflicts with the host's use of the card.

**Warning:** Do NOT download VBIOS ROMs from unverified internet sources. Using an incorrect or malicious VBIOS can render your GPU unusable or cause system instability. Always try to obtain the VBIOS from your own hardware.

There are two primary methods for obtaining a VBIOS, depending on your GPU and system type:

### Method 1: Dumping VBIOS from a Live System (Typically for Desktops/Muxed GPUs)

This method involves dumping the VBIOS directly from your GPU while the host system is running. It's commonly used for Nvidia GPUs on desktop systems. AMD GPU users *may* sometimes skip VBIOS dumping/patching if passthrough works without it, but Nvidia users generally cannot.

**Steps (primarily for Nvidia GPUs using NVFlash):**

1.  **Download NVFlash:**
    Obtain the latest **Linux** version of Nvidia NVFlash from a reputable source like TechPowerUp.
    *   Example link from `README.md`: [Nvidia NVFlash on TechPowerUp](https://www.techpowerup.com/download/nvidia-nvflash/)
    Extract the downloaded archive (e.g., to `~/Downloads/nvflash`).

2.  **Prepare to Dump:**
    Dumping the VBIOS usually requires stopping your desktop environment and unloading the GPU drivers so NVFlash can access the hardware directly.
    *   **Switch to a TTY (Text Terminal):** Press `CTRL + ALT + F3` (or F2, F4, etc.). Log in with your username and password.
    *   **Stop your Display Manager:** The command depends on your display manager:
        *   SDDM (KDE): `sudo systemctl stop sddm`
        *   GDM (GNOME): `sudo systemctl stop gdm` (or `gdm3` on older systems)
        *   LightDM: `sudo systemctl stop lightdm`
        *   LXDM: `sudo systemctl stop lxdm`

3.  **Unload Nvidia Drivers:**
    Execute the following commands to unload the Nvidia kernel modules:
    ```bash
    sudo rmmod nvidia_uvm
    sudo rmmod nvidia_drm
    sudo rmmod nvidia_modeset
    sudo rmmod nvidia
    ```
    If any of these fail with "module not found," it's usually okay, it just means that particular module wasn't loaded.

4.  **Dump the VBIOS:**
    *   Navigate to the directory where you extracted NVFlash:
        ```bash
        cd ~/Downloads/nvflash # Or your actual path
        ```
    *   Make NVFlash executable:
        ```bash
        sudo chmod +x nvflash
        ```
    *   Save the VBIOS ROM:
        ```bash
        sudo ./nvflash --save vbios.rom
        ```
    If successful, a file named `vbios.rom` will be created in the `nvflash` directory. This is your GPU's VBIOS.

5.  **Reload Drivers and Restart Display Manager (or Reboot):**
    You can either reboot the system (simplest):
    ```bash
    sudo reboot
    ```
    Or, attempt to reload drivers and restart the display manager (less reliable):
    ```bash
    sudo modprobe nvidia
    sudo modprobe nvidia_modeset # Order might matter, try variations if needed
    sudo modprobe nvidia_drm
    sudo modprobe nvidia_uvm
    sudo systemctl start sddm # Or your specific display manager
    ```
    If you get back to your desktop, switch back from the TTY (usually `CTRL + ALT + F1` or `CTRL + ALT + F7`).

### Method 2: Extracting VBIOS from System Firmware (Primarily for Muxless Laptops)

Muxless laptops often don't initialize the dGPU's VBIOS in a way that's easily dumpable via NVFlash when the iGPU is active. The dGPU's VBIOS is usually part of the main system BIOS/UEFI firmware update package provided by the laptop manufacturer.

This process is more involved and relies on tools to dissect the firmware update.

**Steps (as outlined in `README-Muxless.md`):**

1.  **Download Laptop BIOS/UEFI Update:**
    Go to your laptop manufacturer's support website and download the latest BIOS update executable (often a `.exe` file for Windows, but we'll extract it under Linux).

2.  **Install Necessary Tools:**
    You'll need several tools. Installation commands vary by distro. For Debian/Ubuntu, you might use `sudo apt install package-name`. For Arch, `sudo pacman -S package-name`. For NixOS, add them to your configuration.
    *   **`innoextract`**: To extract files from Inno Setup installers (many BIOS updaters use this).
    *   **`p7zip`**: For 7zip archives.
    *   **`ruby`, `ruby-dev`, `ruby-bundler`**: For `VBiosFinder`.
    *   **`upx`**: For unpacking some executables.
    *   **`cmake`**: For building some of the tools.
    *   **Build essentials**: `gcc`, `make`, etc.

3.  **Setup `VBiosFinder` and its Dependencies:**
    *   **`VBiosFinder`:**
        ```bash
        git clone https://github.com/coderobe/VBiosFinder.git
        cd VBiosFinder
        # Move your downloaded laptop BIOS update .exe into this VBiosFinder directory
        mv /path/to/your/biosupdate.exe .
        bundle update --bundler
        bundle install --path=vendor/bundle # Installs Ruby gems locally
        cd ..
        ```
    *   **`rom-parser` (for `VBiosFinder`):**
        ```bash
        git clone https://github.com/awilliam/rom-parser.git
        cd rom-parser
        make
        # Move the compiled rom-parser to VBiosFinder's 3rdparty directory
        mv rom-parser ../VBiosFinder/3rdparty/
        cd ..
        ```
    *   **`UEFIExtract` (from `UEFITool`, for `VBiosFinder`):**
        ```bash
        # UEFITool's new_engine branch has UEFIExtract as a standalone tool
        git clone https://github.com/LongSoft/UEFITool.git -b new_engine uefitool_new_engine
        cd uefitool_new_engine
        # Build UEFIExtract (check UEFITool's README for precise build steps, might need Qt tools or can be built headless)
        # The README-Muxless.md implies a direct `cmake UEFIExtract && make` but it's part of the larger UEFITool project.
        # Assuming a command like below (you may need to adapt based on UEFITool build system):
        cmake build/UEFIExtract # This is a guess, refer to UEFITool docs
        make -C build/UEFIExtract # This is a guess
        # Move the compiled uefiextract to VBiosFinder's 3rdparty directory
        mv build/UEFIExtract/uefiextract ../VBiosFinder/3rdparty/UEFIExtract # Adjust path as needed
        cd ..
        ```
        **Note:** Building `UEFIExtract` can be tricky. Consult the UEFITool project for the correct way to build this utility. The `README-Muxless.md` might have simplified these steps.

4.  **Extract the VBIOS using `VBiosFinder`:**
    Navigate into the `VBiosFinder` directory:
    ```bash
    cd VBiosFinder
    ./vbiosfinder extract biosupdate.exe # Replace biosupdate.exe with your actual file name
    ```
    Check the `output/` subdirectory. `VBiosFinder` will attempt to find and extract VBIOS ROMs.
    ```bash
    ls output/
    # Example: vbios_10de_1c94_lenovo.rom, vbios_10de_1c8d.rom
    ```
    You need to identify the correct VBIOS for your dGPU. Match the PCI Vendor and Device ID (e.g., `10de:1c94`) with the ones you found using `lspci -nnk` (see Phase 1.5). Subsystem IDs can also help if multiple VBIOS versions for the same GPU chip exist.
    Copy the correct VBIOS ROM to a safe place, e.g., your home directory, and rename it to `vbios.rom` or similar for clarity.
    ```bash
    cp output/vbios_YOUR_CORRECT_ID.rom ~/vbios_extracted.rom
    ```

### 2.2 VBIOS Patching

Raw VBIOS dumps or extracts often need patching.

#### Patching for Method 1 Dumps (NVFlash)

If you dumped your VBIOS using NVFlash (Method 1), it might contain a UEFI header that can cause issues when used by QEMU. This header needs to be removed.

1.  **Install a Hex Editor:** `Okteta` is recommended in `README.md`.
    ```bash
    sudo pacman -S okteta # Arch
    # sudo apt install okteta # Debian/Ubuntu
    ```
2.  **Backup your VBIOS:** Make a copy of `vbios.rom` (e.g., `vbios_original.rom`) before editing.
3.  **Edit with Okteta:**
    *   Open the *copy* (e.g., `vbios.rom` or `vbios_copy.rom`) in Okteta.
    *   Search for the string `VIDEO` (case-sensitive, as characters).
    *   The `README.md` instructs: "place your cursor **before the first U** before the `VIDEO` value you found and select everything before it. Hit the **INS** (Insert) key to switch Okteta to Edit mode and with everything selected before the **U** hit the "Delete" key on your keyboard."
        This means you are deleting the header portion of the ROM up to where the actual VBIOS code (often marked by a signature starting with 'U') begins.
    *   Save the modified file as a new copy, e.g., `vbios_patched.rom`.

4.  **Move the Patched VBIOS (Optional but common practice for `README.md` method):**
    The `README.md` guide places the patched VBIOS in a system-wide directory. This is primarily if you intend to reference it directly via the `romfile` option in QEMU/libvirt XML *without* patching OVMF.
    ```bash
    sudo mkdir -p /usr/share/vgabios
    sudo cp vbios_patched.rom /usr/share/vgabios/
    # Set permissions (optional, but good practice)
    sudo chmod 644 /usr/share/vgabios/vbios_patched.rom
    # sudo chown yourusername:yourusername /usr/share/vgabios/vbios_patched.rom # If QEMU runs as your user
    sudo chown root:root /usr/share/vgabios/vbios_patched.rom # If QEMU runs as root
    ```

#### Patching for Method 2 Extracts (Muxless Laptops - OVMF Integration)

For muxless laptops, the extracted VBIOS (`vbios_extracted.rom`) is often integrated directly into a custom-built OVMF (UEFI firmware for the VM). This process is detailed in `README-Muxless.md` and involves modifying EDK2 (the UEFI development kit) source code.

**This is an advanced procedure and primarily relevant if you are following the muxless laptop passthrough path that requires a custom OVMF with an embedded VBIOS and ACPI modifications.**

**Overview of the OVMF Patching Process (from `README-Muxless.md`):**
This process aims to make the dGPU's VBIOS available to the VM via ACPI tables embedded in the UEFI firmware itself, which can help initialize the card correctly on muxless systems.

1.  **Prerequisites:**
    *   Install EDK2 development tools: `git`, `python3`, `iasl` (Intel ACPI Compiler), `nasm`, `gcc`, `uuid-dev`.
    *   The extracted VBIOS ROM (e.g., `~/vbios_extracted.rom`).
    *   The `QemuFwCfgAcpi.c` file from this repository (it's a modified EDK2 source file).
    *   The `ssdt1.dat` fake battery ACPI table (linked in `README-Muxless.md`).

2.  **Setup EDK2:**
    It's recommended to place EDK2 in a permanent location like `/opt/edk2` because OVMF builds should not be moved once compiled.
    ```bash
    sudo su # Become root if installing in /opt
    cd /opt
    git clone https://github.com/tianocore/edk2.git
    cd edk2
    git submodule update --init # Pulls in required submodules
    make -C BaseTools # Compile EDK2 build tools
    . ./edksetup.sh BaseTools # Set up EDK2 build environment (note the leading dot)
    exit # If you used sudo su
    ```

3.  **Prepare VBIOS and ACPI Table Source:**
    Navigate to `edk2/OvmfPkg/AcpiPlatformDxe/` (e.g., `/opt/edk2/OvmfPkg/AcpiPlatformDxe/`).
    *   Convert your VBIOS ROM (`~/vbios_extracted.rom`) to a C header file (`vrom.h`):
        ```bash
        # In /opt/edk2/OvmfPkg/AcpiPlatformDxe/
        xxd -i ~/vbios_extracted.rom vrom.h
        ```
    *   Edit `vrom.h`:
        *   Rename the `unsigned char` array to `VROM_BIN`.
        *   Rename the `unsigned int` length variable to `VROM_BIN_LEN`. Note down its value (e.g., `238080`).
    *   Download the sample `ssdt.asl` (ACPI Source Language) file (linked as `ssdt.txt` in `README-Muxless.md`):
        ```bash
        # In /opt/edk2/OvmfPkg/AcpiPlatformDxe/
        wget https://github.com/jscinoz/optimus-vfio-docs/files/1842788/ssdt.txt -O ssdt.asl
        ```
    *   Edit `ssdt.asl`:
        *   Change line 37 (`Name (RVBS, <size>)`) to use your `VROM_BIN_LEN` value.
            Example: `Name (RVBS, 238080)`
    *   Compile `ssdt.asl` and generate `vrom_table.h`:
        ```bash
        # In /opt/edk2/OvmfPkg/AcpiPlatformDxe/
        iasl -f ssdt.asl # Compiles ssdt.asl to Ssdt.aml. Ignore some errors if Ssdt.aml is created.
        xxd -c1 Ssdt.aml | tail -n +37 | cut -f2 -d' ' | paste -sd' ' | sed 's/ //g' | xxd -r -p > vrom_table.aml
        xxd -i vrom_table.aml | sed 's/vrom_table_aml/vrom_table/g' > vrom_table.h
        ```
    *   Copy `vrom.h` and `vrom_table.h` to `edk2/OvmfPkg/Library/AcpiPlatformLib/`:
        ```bash
        sudo cp vrom.h vrom_table.h /opt/edk2/OvmfPkg/Library/AcpiPlatformLib/
        ```

4.  **Replace `QemuFwCfgAcpi.c`:**
    Copy the provided `QemuFwCfgAcpi.c` from this GPU passthrough repository into the EDK2 source tree, overwriting the original:
    ```bash
    # Assuming QemuFwCfgAcpi.c from the cloned gpu-passthrough repo is in ~/gpu-passthrough/
    sudo cp ~/gpu-passthrough/QemuFwCfgAcpi.c /opt/edk2/OvmfPkg/Library/AcpiPlatformLib/
    ```

5.  **Configure EDK2 Build Target:**
    Edit `/opt/edk2/Conf/target.txt` and set:
    ```
    ACTIVE_PLATFORM       = OvmfPkg/OvmfPkgX64.dsc
    TARGET_ARCH           = X64
    TOOL_CHAIN_TAG        = GCC5 # Or your compiler, e.g., GCC5, CLANGPDB
    ```

6.  **Build OVMF:**
    Navigate to the root of your EDK2 directory (e.g., `/opt/edk2/`).
    Source the setup script again if in a new terminal:
    ```bash
    # cd /opt/edk2/
    # . ./edksetup.sh BaseTools # If needed
    ```
    Run the build command:
    ```bash
    build
    ```
    If successful, the compiled OVMF files will be in `Build/OvmfX64/DEBUG_GCC5/FV/` (or similar path depending on `TOOL_CHAIN_TAG` and build type like `RELEASE`). The key files are `OVMF_CODE.fd` and `OVMF_VARS.fd`.
    **Crucially, do not move these compiled OVMF files.** Libvirt will be configured to point to them in their build location.

This VBIOS handling phase is critical. The method you choose and the success of patching heavily influence the outcome of your GPU passthrough setup.

---

## Phase 3: Libvirt Hook Scripts

Libvirt hook scripts are used to automate tasks when a VM starts or stops. For GPU passthrough, they are essential for:

*   **Detaching the GPU from the host:** Before the VM starts, the GPU (and its associated devices like the audio controller) needs to be unbound from host drivers (e.g., `nvidia`, `nouveau`, `snd_hda_intel`) and bound to `vfio-pci` if not already done via early binding (Phase 1.5). Even with early binding, these scripts can perform additional cleanup or ensure the state is correct.
*   **Reattaching the GPU to the host:** After the VM shuts down, the scripts rebind the GPU to its original host drivers, making it usable by the host system again.
*   **Preventing system sleep/hibernate issues:** Some passthrough setups are sensitive to host power state changes. The hooks can engage services to prevent this while the VM is running.

This repository provides a set of hook scripts and an installer for them, as detailed in `README.md`.

### 3.1 Understanding the Provided Hook Scripts

*   **`hooks/vfio-startup.sh` (or `/bin/vfio-startup.sh` after installation):**
    *   This script runs *before* the VM starts.
    *   Its primary job is to unbind the GPU and its associated devices from their host drivers.
    *   It then ensures these devices are bound to the `vfio-pci` driver, making them available for QEMU/libvirt to pass to the VM.
    *   It might also include commands to prevent host system sleep (e.g., by using `systemd-inhibit` or a custom service like `libvirt-nosleep@.service`).

*   **`hooks/vfio-teardown.sh` (or `/bin/vfio-teardown.sh` after installation):**
    *   This script runs *after* the VM shuts down.
    *   It unbinds the GPU devices from `vfio-pci`.
    *   It then attempts to rebind them to their original host drivers (e.g., `nvidia`).
    *   It re-enables host display managers or graphics services if they were stopped.
    *   It releases any sleep inhibitor locks.

*   **`install_hooks.sh`:**
    *   This script automates the installation of the startup/teardown scripts and the main QEMU hook dispatcher.
    *   It typically copies `vfio-startup.sh` and `vfio-teardown.sh` to a system directory (e.g., `/bin/` or `/usr/local/bin/`).
    *   It creates or configures the main QEMU hook dispatcher script at `/etc/libvirt/hooks/qemu`.
    *   It may also install a systemd service like `libvirt-nosleep@.service` (found in the `systemd-no-sleep` directory) to manage sleep inhibition.

### 3.2 Installing the Hook Scripts

If you haven't already, clone this repository:
```bash
git clone https://github.com/0xRama/gpu-passthrough # Or your fork/local path
cd gpu-passthrough
```

Then run the installation script:
```bash
sudo ./install_hooks.sh
```
This script will perform the necessary setup steps.

**Verify Installed Files (as per `README.md`):**
After running `install_hooks.sh`, check that the following files/directories have been created or correctly configured:

*   `/etc/systemd/system/libvirt-nosleep@.service` (or similar if your system uses a different init system)
*   `/bin/vfio-startup.sh` (or the path chosen by the script)
*   `/bin/vfio-teardown.sh` (or the path chosen by the script)
*   `/etc/libvirt/hooks/qemu` (This is the main dispatcher script)

Make sure the scripts in `/bin/` (or equivalent) are executable (`chmod +x`). The `install_hooks.sh` should handle this.

### 3.3 Configuring the QEMU Hook Dispatcher

The file `/etc/libvirt/hooks/qemu` acts as a dispatcher. When any QEMU VM managed by libvirt starts or stops, libvirt executes this script, passing arguments like the VM name and the operation (e.g., `prepare`, `start`, `release`).

The dispatcher script needs to be configured to call your specific `vfio-startup.sh` and `vfio-teardown.sh` scripts only for the VM(s) that will use GPU passthrough.

1.  **Edit the QEMU hook dispatcher:**
    ```bash
    sudo nvim /etc/libvirt/hooks/qemu
    ```

2.  **Modify the VM name check:**
    The `install_hooks.sh` likely sets up a basic structure. You need to ensure it targets your passthrough VM.
    The `README.md` suggests an initial placeholder modification:
    Locate a line similar to: `if [[ $OBJECT == "win10" || $OBJECT == "somevm" ]]; then`
    (Where `$OBJECT` is the variable holding the VM name passed by libvirt).

    *   **Change `"somevm"` or `"win10"` to the actual name of your GPU passthrough VM.** For example, if your VM will be named `win11-gpu`:
        ```bash
        # Inside /etc/libvirt/hooks/qemu
        VM_NAME="$1" # First argument to the script is the VM name
        OPERATION="$2" # Second argument is the phase (e.g., prepare, start, stopped, release)

        # Example from README.md style, assuming $OBJECT is set to VM_NAME earlier in the script
        # if [[ $OBJECT == "win11-gpu" ]]; then

        # A more common structure within the qemu hook script:
        if [ "$VM_NAME" = "win11-gpu" ]; then
            if [ "$OPERATION" = "prepare" ] || [ "$OPERATION" = "start" ]; then
                # (Original README.md structure has a single script call here)
                # This is where vfio-startup.sh logic would be invoked
                /bin/vfio-startup.sh
            elif [ "$OPERATION" = "stopped" ] || [ "$OPERATION" = "release" ]; then
                # This is where vfio-teardown.sh logic would be invoked
                /bin/vfio-teardown.sh
            fi
        fi
        ```

    *   **Multiple VMs:** If you have multiple VMs that will use these hooks (e.g., for different passed-through GPUs or the same GPU at different times), you can use `||` (OR operator):
        ```bash
        if [ "$VM_NAME" = "win11-gpu" ] || [ "$VM_NAME" = "linux-gaming-gpu" ]; then
            # ... same logic ...
        fi
        ```

    **Important:** The exact internal structure of the `/etc/libvirt/hooks/qemu` script installed by `install_hooks.sh` needs to be respected. The goal is to ensure that the `vfio-startup.sh` logic runs *before* your target VM starts, and `vfio-teardown.sh` logic runs *after* it stops.

    The `README.md` initially has you set this to a placeholder like `somevm` during VM creation and then update it to the final VM name *after* the VM is fully set up but *before* the first boot with the GPU passed through. This is a safe approach to avoid the hooks running prematurely.

### 3.4 Considerations for Muxless Setups

While `README-Muxless.md` doesn't explicitly detail general-purpose hook scripts like `vfio-startup.sh` and `vfio-teardown.sh` (as it leans on NixOS configuration and specific OVMF/QEMU XML settings for GPU state management), the principles can still apply if needed:

*   **Driver Unbinding/Rebinding:** If the `vfio-pci.ids` kernel parameter or other early binding methods are not reliably keeping the dGPU isolated on the muxless host, startup/teardown hooks could enforce this.
*   **Display Management:** If using Looking Glass and you need to manage host display services or blacklisting/unblacklisting the GPU from the host X server, hooks could automate this.

However, many muxless guides rely on the custom OVMF and specific QEMU XML configurations (like `<rom bar='off'/>` or vendor/device ID spoofing) to manage the GPU state without needing complex host-side unbinding/rebinding scripts *during VM operation*, as the GPU is often intended to be fully dedicated to the VM once passthrough is active.

If you are primarily following the muxless path with a custom OVMF, you might find these general-purpose hook scripts less critical or requiring adaptation. The `vfio-pci.ids` kernel parameter should ideally handle the host driver detachment.

**The `libvirt-nosleep@.service` is generally beneficial for all passthrough types to prevent the host from sleeping while the VM is active.**

This phase ensures that the host system is prepared to dynamically hand over control of the GPU to the VM and reclaim it afterward.

---

## Phase 4: Virtual Machine Creation & Configuration

This phase covers creating the virtual machine and performing initial configurations before passing through the GPU. We'll start with general VM creation steps, then detail specific configurations crucial for GPU passthrough, especially for muxless setups.

### 4.1 Download Necessary ISOs and Drivers

Before creating the VM, download the following:

1.  **Operating System ISO:**
    *   **Windows:** Download your desired Windows version (e.g., Windows 10, Windows 11) directly from Microsoft.
        *   Windows 11: [Microsoft Software Download Page](https://www.microsoft.com/software-download/windows11)
        *   Windows 10: [Microsoft Software Download Page](https://www.microsoft.com/software-download/windows10)
    *   **Linux:** Download the ISO for your chosen Linux distribution (e.g., Arch Linux, Ubuntu).
        *   Arch Linux: [Arch Linux Downloads](https://archlinux.org/download/)

2.  **VirtIO Drivers for Windows:**
    These drivers are essential for paravirtualized devices (like network and disk controllers) in QEMU/KVM, providing significantly better performance than emulated hardware. Windows does not include these by default.
    *   Download the latest **stable VirtIO ISO** from the official Fedora project page:
        [VirtIO Win ISO Direct Downloads](https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/stable-virtio/virtio-win.iso)
    *   The `README-Muxless.md` also references `virtio-win-pkg-scripts` which can be used to build custom VirtIO driver packages: [virtio-win-pkg-scripts on GitHub](https://github.com/virtio-win/virtio-win-pkg-scripts/blob/master/README.md)

3.  **Additional Files for Muxless/Looking Glass Setup (from `README-Muxless.md`):**
    If you are planning a muxless laptop setup with Looking Glass, you'll also need:
    *   **Spice Guest Tools:** For improved Spice display interaction (if used before Looking Glass is active).
        [Spice Guest Tools Download](https://www.spice-space.org/download/windows/spice-guest-tools/spice-guest-tools-latest.exe)
    *   **Fake Display Driver (USBMMIDD_v2):** To create a virtual display in Windows for Looking Glass when no physical display is attached to the dGPU in the VM.
        [USBMMIDD_v2 Download (Amyuni)](https://www.amyuni.com/downloads/usbmmidd_v2.zip)
    *   **WinFSP (Windows File System Proxy):** For `virtiofs` shared folders between the Windows VM and Linux host.
        Download the MSI version: [WinFSP Releases on GitHub](https://github.com/winfsp/winfsp/releases)
    *   **Fake Battery SSDT (`ssdt1.dat`):** An ACPI table to simulate a battery in the VM, which can help bypass Nvidia's Code 43 error on some GeForce GPUs in VMs.
        [ssdt1.dat Download (lantian.pub)](https://lantian.pub/usr/uploads/202007/ssdt1.dat) (This file is also referenced in the `win11.xml` for QEMU command-line arguments.)
    *   **Looking Glass Host Application (Windows Binary):** The application that runs inside the Windows VM to capture the screen.
        Download the version that matches the client you'll use (e.g., B7-rc1 was mentioned). [Looking Glass Downloads](https://looking-glass.io/downloads)

Store all downloaded ISOs and files in a known location (e.g., `~/Downloads/isos`, `~/vm_stuff/`).

### 4.2 General VM Creation with `virt-manager` (Baseline Setup)

These steps follow the `README.md` guide for creating a basic VM structure using Virtual Machine Manager (`virt-manager`). We will customize this extensively later, especially for muxless setups.

1.  **Open `virt-manager`:** Launch Virtual Machine Manager from your applications menu or by typing `virt-manager` in a terminal.
2.  **Create a New VM:** Click the "Create a new virtual machine" icon (often a computer monitor with a plus sign).
3.  **Step 1: Choose Installation Method:**
    *   Select "Local install media (ISO image or CDROM)".
    *   Click "Forward".
4.  **Step 2: Locate Install Media:**
    *   Click "Browse..." then "Browse Local".
    *   Navigate to and select your downloaded OS ISO (e.g., Windows, Linux).
    *   Ensure "Automatically detect OS based on media" is checked, or manually select the OS type if needed.
    *   Click "Forward".
5.  **Step 3: Memory and CPU Settings:**
    *   **Memory (RAM):** Allocate a reasonable amount of RAM. For a Windows gaming VM, 8GB (8192 MB) is a common minimum, with 16GB (16384 MB) being better. `README.md` suggests half your system RAM. `README-Muxless.md` suggests capping at 80% of total system RAM if you experience freezes.
    *   **CPUs:** Allocate a number of vCPUs. This should ideally be based on your physical CPU's core/thread count. For example, if you have an 8-core/16-thread CPU, you might pass 8 or 12 vCPUs. `README.md` suggests matching your system's logical CPU count initially.
    *   Click "Forward".
6.  **Step 4: Enable Storage:**
    *   Choose "Create a disk image for the virtual machine".
    *   Set the size (e.g., 100GB, 256GB, or more, depending on your needs). Using QCOW2 format allows for sparse allocation (thin provisioning).
    *   Click "Forward".
7.  **Step 5: Ready to Begin Installation:**
    *   **Name your VM:** Choose a descriptive name (e.g., `win11-passthrough`, `muxless-win11`). This name will be used by libvirt hooks and commands.
    *   **Crucially, check the box: "Customize configuration before install".**
    *   Click "Finish".

### 4.3 Customizing VM Configuration (Pre-OS Install)

After clicking "Finish" in the previous step, the VM configuration window will open. Make the following adjustments before starting the OS installation. **Click "Apply" after each significant change, especially when changing tabs in `virt-manager`.**

#### 4.3.1 Overview Tab

*   **Firmware:** This is critical.
    *   **For most setups (including general `README.md` path):** Change Firmware to **UEFI x86_64: /usr/share/OVMF/OVMF_CODE.fd** (or similar path for your distribution's standard OVMF package). This is standard UEFI firmware.
    *   **For muxless setups requiring a custom VBIOS-patched OVMF (from Phase 2.2, Method 2):**
        You must point to your custom-built `OVMF_CODE.fd`. In the XML (or if `virt-manager` allows specifying a custom path directly):
        `<loader readonly='yes' type='pflash'>/opt/edk2/Build/OvmfX64/DEBUG_GCC5/FV/OVMF_CODE.fd</loader>`
        And you'll also need to specify the variables file, usually by copying the `OVMF_VARS.fd` from your build location to libvirt's domain-specific NVRAM store or referencing it. `virt-manager` might create a copy automatically if you just specify the `OVMF_CODE.fd`. The `win11.xml` example uses:
        `<nvram template='/opt/edk2/Build/OvmfX64/DEBUG_GCC5/FV/OVMF_VARS.fd'>/var/lib/libvirt/qemu/nvram/win11_VARS.fd</nvram>`
        **If `virt-manager` doesn't let you select a custom path easily, you'll need to edit the XML later (see section 4.4).** For now, select the default OVMF if it's the only option, and make a note to change it.
*   **Chipset:** Should default to something like `Q35` or `i440FX`. `Q35` is generally preferred for modern passthrough setups as it more closely resembles modern hardware and provides PCIe ACS support. If you have a choice, pick `Q35`.

#### 4.3.2 CPUs Tab

*   **vCPU Allocation:** Confirm the number of vCPUs. Consider your host's needs.
*   **CPU Model:** You can often leave this as the default (`host-model` or `host-passthrough` if available in XML later) for best compatibility and performance. `host-passthrough` exposes all host CPU features.
*   **Topology (Optional):** You can manually specify sockets, cores, threads to match your physical CPU topology more closely. This can sometimes improve performance or compatibility.

#### 4.3.3 Memory Tab

*   Confirm current allocation. Max allocation should also be set to this value unless you plan on memory ballooning (not typically done with fixed passthrough setups).

#### 4.3.4 Boot Options Tab

*   Check "Enable boot menu".
*   Ensure your **SATA CDROM 1** (or IDE CDROM if that's what `virt-manager` created for the OS ISO) is at the top of the boot order, or at least before the virtual disk.
*   Later, you'll add the VirtIO drivers ISO as another CDROM. Make sure the OS ISO boots first for installation.

#### 4.3.5 Disks (SATA Disk 1 / VirtIO Disk 1)

*   The primary virtual disk is likely `SATA Disk 1` if `virt-manager` created it that way. For best performance, it should be `VirtIO Disk`.
*   **If it's SATA:**
    *   Expand "Advanced options".
    *   Disk bus: Change from `SATA` to `VirtIO`.
    *   Cache mode (under Performance options): `README.md` suggests `writeback`. Other options include `none` (safer for data integrity on host crash but slower) or `unsafe` (fastest, risk of data loss). `default` or `none` are common. `writeback` can be good for performance.
*   **If it's already VirtIO Disk 1:**
    *   Confirm Cache mode settings under "Performance options".
    *   Discard mode: Set to `unmap` (if your underlying storage supports it, e.g., SSD with TRIM) to allow TRIM commands from the guest to reclaim space on the host.

#### 4.3.6 Network Interface (NIC)

*   Device model: Should be `virtio` for best performance.
*   Network source: Usually `Virtual network 'default': NAT`. This is fine for most initial setups.
*   `README-Muxless.md` suggests removing the network interface initially to bypass Windows 11 online account creation during setup. You can do this by selecting the NIC and clicking "Remove", then adding it back after Windows is installed.

#### 4.3.7 Add Hardware (for VirtIO Drivers ISO - Windows Guests)

If installing Windows, you need to attach the VirtIO drivers ISO.

*   Click "Add Hardware" at the bottom left.
*   Select "Storage".
*   Device type: "CD/DVD ROM device".
*   Bus type: `SATA` (or `IDE` if other CDROM is IDE). Click "Finish".
*   A new "SATA CDROM 2" will appear. Select it.
*   Connection: "Managed or other existing image".
*   Click "Browse..." -> "Browse Local" and select your `virtio-win.iso` file.
*   Click "Apply".

#### 4.3.8 TPM (for Windows 11)

Windows 11 requires a TPM 2.0 module. `README.md` mentions this.

*   **Install `swtpm` on the host:**
    ```bash
    sudo pacman -S swtpm # Arch
    # sudo apt install swtpm # Debian/Ubuntu
    ```
*   **Add TPM device in `virt-manager`:**
    *   Click "Add Hardware".
    *   Select "TPM".
    *   Model: `CRB` (or `TIS`, `CRB` is more modern).
    *   Type: `Emulated device`.
    *   Version: `2.0`.
    *   Backend device: Usually `/dev/tpm0` if you have a physical TPM, or select an emulated one. For a purely virtual TPM using `swtpm`, `virt-manager` should handle the creation of a `swtpm` instance.
    *   Click "Finish".

At this point, your VM has a basic hardware configuration suitable for OS installation. The GPU is NOT yet passed through.

### 4.4 Advanced XML Configuration (Primarily for Muxless Laptops)

For muxless laptop setups, `virt-manager` often doesn't expose all necessary options. Direct XML editing is required. The `win11.xml` file in this repository serves as a template for such a configuration.

**To edit XML in `virt-manager`:**
Select your VM, go to "Edit" -> "Preferences" and check "Enable XML editing". Then, for your VM, the "Overview" tab will have an "XML" sub-tab.
Alternatively, use `sudo virsh edit YOUR_VM_NAME`.

**Key XML Sections to Review/Modify (based on `win11.xml` and `README-Muxless.md`):**

*   **`<domain type='kvm' xmlns:qemu='http://libvirt.org/schemas/domain/qemu/1.0'>`**: Ensure `xmlns:qemu` is present if you use QEMU-specific command-line arguments or features.
*   **`<name>YOUR_VM_NAME</name>`**
*   **`<uuid>...</uuid>`**: Libvirt generates this.
*   **`<memory unit='KiB'>16777216</memory>`**: (e.g., 16GB). Adjust as needed.
*   **`<currentMemory unit='KiB'>16777216</currentMemory>`**
*   **`<vcpu placement='static'>8</vcpu>`**: (e.g., 8 vCPUs).
*   **`<os>` section:**
    *   **`<type arch='x86_64' machine='pc-q35-7.0'>hvm</type>`**: Or your latest Q35 machine type.
    *   **`<loader readonly='yes' type='pflash'>/opt/edk2/Build/OvmfX64/DEBUG_GCC5/FV/OVMF_CODE.fd</loader>`**: **CRITICAL for muxless with patched OVMF.** Points to your custom built OVMF code.
    *   **`<nvram template='/opt/edk2/Build/OvmfX64/DEBUG_GCC5/FV/OVMF_VARS.fd'>/var/lib/libvirt/qemu/nvram/YOUR_VM_NAME_VARS.fd</nvram>`**: Points to your custom OVMF variables template, and libvirt will create a VM-specific copy.
    *   `<boot dev='hd'/>` (or `cdrom` initially)
*   **`<features>`:**
    *   `<acpi/>`, `<apic/>`, `<hyperv mode='custom'>...</hyperv>`, `<kvm><hidden state='on'/></kvm>` (for hiding KVM virtualization from guest, can help with some anti-cheat/DRM).
    *   The `win11.xml` has specific Hyper-V enlightenments. Copying these can be beneficial for Windows guests.
*   **`<cpu mode='host-passthrough' check='none' migratable='off'>`**: `host-passthrough` is good. `migratable='off'` is typical for passthrough.
    *   `<topology sockets='1' dies='1' cores='4' threads='2'/>` (Example for 4 cores, 2 threads per core = 8 vCPUs). Adjust to your CPU.
    *   `<feature policy='require' name='invtsc'/>` etc. (specific CPU features from `win11.xml`)
*   **`<clock offset='localtime'>...</clock>`**: Important for time synchronization.
*   **`<devices>` section:**
    *   **`<emulator>/usr/bin/qemu-system-x86_64</emulator>`**: Path to your QEMU emulator. `README-Muxless.md` notes to change this via `which qemu-system-x86_64`.
    *   **Disks (`<disk type='file' device='disk'>`):**
        *   `<driver name='qemu' type='qcow2' discard='unmap'/>`
        *   `<source file='/var/lib/libvirt/images/YOUR_VM_NAME.qcow2'/>` (Path to your VM's disk image)
        *   `<target dev='vda' bus='virtio'/>`
        *   `<boot order='1'/>` (If booting from this disk)
    *   **CD-ROMs (`<disk type='file' device='cdrom'>`):**
        *   For OS ISO: `<source file='/path/to/your/windows.iso'/>`, `<target dev='sda' bus='sata'/>`, `<boot order='2'/>` (or higher priority for install).
        *   For VirtIO ISO: `<source file='/path/to/your/virtio-win.iso'/>`, `<target dev='sdb' bus='sata'/>`.
    *   **Controllers (`<controller type='usb' .../>`, `<controller type='pci' .../>`, etc.):** `virt-manager` usually adds these. `win11.xml` might have specific models or PCI addresses; compare if facing issues.
    *   **`<interface type='network'>` (Network):**
        *   `<source network='default'/>`
        *   `<model type='virtio'/>`
    *   **`<graphics type='spice' autoport='yes'>...</graphics>` / `<video>...</video>`:**
        These are for the initial emulated display. You will REMOVE these in Phase 5 when passing through the real GPU. For initial OS setup, they are needed.
        `win11.xml` might already have these removed or configured for Looking Glass (e.g. `type='none'` for graphics).
    *   **Sound (`<sound model='ich9'>`):** Emulated sound, can be removed later if GPU audio is used.
    *   **VirtioFS (Shared Folder - from `README-Muxless.md`):**
        ```xml
        <filesystem type='mount' accessmode='passthrough'>
          <driver type='virtiofs'/>
          <binary path='/usr/lib/qemu/virtiofsd'/> <!-- Path from 'which virtiofsd' -->
          <source dir='/path/on/host/to/share'/>
          <target dir='shared_folder_name_in_guest'/>
        </filesystem>
        ```
*   **QEMU Command Line (`<qemu:commandline>`):**
    This is for passing custom arguments to QEMU not directly supported by libvirt's schema.
    *   **Fake Battery (`ssdt1.dat`) from `win11.xml`:**
        ```xml
        <qemu:commandline>
          <qemu:arg value='-acpitable'/>
          <qemu:arg value='file=/path/to/your/ssdt1.dat'/>
        </qemu:commandline>
        ```
    *   **GPU Vendor/Device ID Hiding/Spoofing (from `win11.xml`, CRITICAL for Nvidia Code 43 on some cards):**
        This section is typically added when the actual PCI device for the GPU is passed through. It tells QEMU to override certain PCI configuration space values for the passed-through device.
        You'll add the `<hostdev>` for your GPU first (Phase 5), then add this `qemu:override` for that device.
        ```xml
        <qemu:override>
          <qemu:device alias="hostdev0"> <!-- 'hostdev0' must match the alias of your passed-through GPU -->
            <qemu:frontend>
              <!-- Use YOUR GPU's actual IDs here, converted to decimal or hex (0x prefix) -->
              <!-- Example using hex values: -->
              <qemu:property name="x-pci-vendor-id" type="unsigned" value="0x10de"/> <!-- Hex Vendor ID -->
              <qemu:property name="x-pci-device-id" type="unsigned" value="0x1c94"/> <!-- Hex Device ID -->
              <qemu:property name="x-pci-sub-vendor-id" type="unsigned" value="0x17aa"/> <!-- Hex Subsystem Vendor ID -->
              <qemu:property name="x-pci-sub-device-id" type="unsigned" value="0x3f9b"/> <!-- Hex Subsystem Device ID -->
              <!-- README-Muxless.md shows these as decimal values converted from hex. -->
              <!-- It's often easier to use hex directly if QEMU supports it, prefixing with 0x. Check QEMU documentation. -->
              <!-- Example from README-Muxless.md (decimal): -->
              <!-- <qemu:property name="x-pci-vendor-id" type="unsigned" value="4318"/> -->
              <!-- <qemu:property name="x-pci-device-id" type="unsigned" value="7316"/> -->
              <!-- <qemu:property name="x-pci-sub-vendor-id" type="unsigned" value="6058"/> -->
              <!-- <qemu:property name="x-pci-sub-device-id" type="unsigned" value="16283"/> -->
            </qemu:frontend>
          </qemu:device>
        </qemu:override>
        ```
        **Find your GPU's alias:** If you are unsure of the alias (`hostdev0`), you can check the live XML after adding the GPU: `sudo virsh dumpxml YOUR_VM_NAME`. Look for the `<hostdev>` entry for your GPU; it might have an `<alias name='hostdev0'/>` sub-tag.

*   **KVM Hiding ( `<kvm><hidden state='on'/></kvm>` ):**
    Already covered in Phase 4.4, but re-verify it's in the `<features>` section. This can help with some games or applications that detect virtualization.

*   **Hyper-V Enlightenments ( `<hyperv mode='custom'>...</hyperv>` ):**
    Also covered in Phase 4.4. Beneficial for Windows guests. Ensure the relevant enlightenments (`relaxed`, `vapic`, `spinlocks`, `vpindex`, `synic`, `stimer`, `reset`, `frequencies`) are enabled as per `win11.xml` or `README.md` suggestions.

*   **CPU Pinning and HugePages (Advanced Optimizations - Optional for now):**
    `README.md` details CPU pinning (assigning specific host CPU threads to guest vCPUs) and configuring HugePages for better memory performance. These are advanced topics. It's recommended to get basic passthrough working first, then explore these for performance tuning. We can add a dedicated section for these later if desired.

### 4.5 Final XML Tweaks

After making all necessary changes, save your XML configuration. If using `virsh edit`, save and close. In `virt-manager`, click "Apply".

---

## Phase 5: OS Installation, GPU Passthrough & Final VM Tweaks

With the VM configured (but without the GPU passed through yet), it's time to install the guest operating system. After the OS is set up with essential drivers, we'll modify the VM to pass through the dedicated GPU.

### 5.1 Installing the Guest Operating System

1.  **Start the VM:** In `virt-manager`, select your VM and click "Open", then click the "Run" button (play icon).
2.  **Boot from ISO:** The VM should boot from the OS installation ISO you configured (e.g., Windows, Linux).
    *   If it tries to boot from the network or shows an error, ensure the boot order (Phase 4.3.4) is correct with the CDROM containing the OS ISO as the primary boot device.
    *   You might need to press a key quickly (e.g., ESC, F12) to enter the boot menu if `virt-manager`'s boot menu option wasn't effective.

3.  **OS Installation Process:**
    *   **Windows:**
        *   Follow the on-screen prompts.
        *   **Loading VirtIO Drivers:** When you reach the disk selection screen ("Where do you want to install Windows?"), Windows might not see your virtual disk if it's configured as VirtIO. This is expected.
            *   Click "Load driver".
            *   Click "Browse".
            *   Navigate to the VirtIO CDROM (e.g., `E:` or `D:` drive, containing `virtio-win.iso` contents).
            *   Select the appropriate driver folder:
                *   For disk: `viostor/<Your Windows Version>/amd64/` (e.g., `viostor/win11/amd64/`)
                *   Click "OK", then "Next". The VirtIO storage driver will install, and your virtual disk should appear.
            *   If you also need network drivers during install (e.g., for Windows 11 Home which might demand an online account, though we suggested removing the NIC earlier to bypass this), you'd load `NetKVM/<Your Windows Version>/amd64/`.
        *   Proceed with partitioning and installation on the virtual disk.
        *   **Offline Account (Windows 11):** If you did not remove the NIC and are forced into an online account setup, you can sometimes bypass it by typing `OOBE\BYPASSNRO` when it asks for network, or `no@thankyou.com` as the email (these tricks change, so search for current methods if needed). Removing the NIC temporarily (as per Phase 4.3.6) is more reliable for forcing an offline account option.
    *   **Linux:**
        *   Follow the distribution-specific installation guide.
        *   VirtIO drivers are generally included in modern Linux kernels, so your VirtIO disk and network should be detected automatically.

4.  **Initial Guest Setup & VirtIO Drivers (Post-OS Install for Windows):**
    *   Once the OS is installed and boots up for the first time (still on emulated graphics):
    *   **Windows:**
        *   Open Device Manager. You'll likely see several devices with yellow exclamation marks (missing drivers).
        *   Mount the `virtio-win.iso` if it's not already accessible.
        *   Run the `virtio-win-guest-tools.exe` installer from the ISO. This will install all necessary VirtIO drivers (network, balloon, serial, etc.) and the QEMU guest agent.
        *   Alternatively, manually install drivers from the ISO via Device Manager for any remaining unknown devices.
        *   Install Spice guest tools (`spice-guest-tools-latest.exe`) if you plan to use Spice for a better emulated display experience before Looking Glass, or for easier copy-paste if needed. This is more relevant if you kept the Spice graphics adapter.
    *   **Linux:** Ensure the `qemu-guest-agent` is installed and running. This helps with integration features like graceful shutdown from `virt-manager`.
        ```bash
        # Arch
        sudo pacman -S qemu-guest-agent
        sudo systemctl enable --now qemu-guest-agent

        # Debian/Ubuntu
        sudo apt update
        sudo apt install qemu-guest-agent
        sudo systemctl enable --now qemu-guest-agent
        ```

5.  **Install Basic Utilities and Updates:** Install any necessary guest OS updates, web browser, text editor, etc. This is easier to do now with emulated graphics.

6.  **Shut Down the VM:** Once the OS is installed and basic drivers are set up, perform a clean shutdown of the VM.

### 5.2 Passing Through the GPU

This is where we modify the VM to use your actual GPU instead of emulated graphics.

1.  **Ensure VM is Powered Off.**

2.  **Add PCI Host Devices (GPU and its components):**
    *   Open `virt-manager`, select your VM, and click "Open" to view its configuration.
    *   Click "Add Hardware".
    *   Select "PCI Host Device".
    *   You should see a list of PCI devices from your host. Locate your GPU. It will likely have multiple entries (functions):
        *   The **VGA controller** (e.g., `NVIDIA Corporation GP104 [GeForce GTX 1070]`).
        *   The **Audio Device** associated with the GPU (e.g., `NVIDIA Corporation GP104 High Definition Audio Controller`).
        *   Potentially USB-C controllers or other functions on the same card.
    *   **Add the VGA controller first.** Select it and click "Finish".
    *   **Add the Audio Device next.** Click "Add Hardware" -> "PCI Host Device", select the GPU's audio controller, and click "Finish".
    *   If your GPU has other functions (like a USB controller) that you identified in Phase 1 (IOMMU groups) and want to pass through, add them as well. Ensure they are in the same IOMMU group or a group that can be safely passed.

3.  **Remove Emulated Graphics and Related Devices:**
    To ensure the passed-through GPU is the primary (or only) graphics adapter in the VM, remove the emulated ones.
    *   In `virt-manager`, select:
        *   **Display Spice** (or **Display VNC**) and click "Remove".
        *   **Video QXL** (or **Video VGA**, **Video Virtio**, etc.) and click "Remove".
        *   You might also remove the emulated USB Tablet, Keyboard, and Mouse if you plan to pass through a dedicated USB controller or use host USB passthrough directly for these peripherals. However, it's often safer to keep emulated input initially until the GPU is working.
        *   The `win11.xml` example has graphics explicitly set to none: `<graphics type='none'/>` or uses a minimal configuration for Looking Glass like `<graphics type='spice' autoport='yes' listen='127.0.0.1'><listen type='address' address='127.0.0.1'/></graphics>`. For initial GPU passthrough, removing all display/video devices listed under the VM's hardware details is common.

### 5.3 GPU-Specific XML Tweaks

Direct XML editing (`sudo virsh edit YOUR_VM_NAME` or via `virt-manager`'s XML tab) is often necessary for stable GPU passthrough.

*   **VBIOS Path / ROM BAR Off (for cards that need it, often Nvidia):**
    *   If you patched your VBIOS (Phase 2.2.1) and placed it in `/usr/share/vgabios/` or similar, you need to tell QEMU to use it for the passed-through GPU.
    *   Locate your `<hostdev mode='subsystem' type='pci' managed='yes'>` entry for the GPU's VGA controller.
    *   Add a `<rom file='/path/to/your/patched.rom'/>` sub-element.
        ```xml
        <hostdev mode='subsystem' type='pci' managed='yes'>
          <source>
            <address domain='0x0000' bus='0x01' slot='0x00' function='0x0'/>
          </source>
          <rom file='/usr/share/vgabios/patched_vbios.rom'/> <!-- Example path -->
          <address type='pci' domain='0x0000' bus='0x04' slot='0x00' function='0x0'/> <!-- Libvirt assigns guest PCI slot -->
        </hostdev>
        ```
    *   **For some cards (especially newer Nvidia), you might not need to supply a VBIOS file if the one on the card works, OR you might need to explicitly disable QEMU trying to read it with `rom bar='off'` (as seen in `README-Muxless.md` and `win11.xml`):**
        ```xml
        <hostdev mode='subsystem' type='pci' managed='yes'>
          <source>
            <address domain='0x0000' bus='0x01' slot='0x00' function='0x0'/>
          </source>
          <rom bar='off'/>
          <address type='pci' domain='0x0000' bus='0x04' slot='0x00' function='0x0'/>
        </hostdev>
        ```
        **You generally use either `<rom file='...'/>` OR `<rom bar='off'/>`, not both.** The choice depends on your GPU and what works. For muxless setups with VBIOS in OVMF, `rom bar='off'` is common for the dGPU passthrough entry in the VM's XML.

*   **Vendor ID Spoofing (Nvidia Code 43 fix, from `win11.xml` and `README-Muxless.md`):**
    This is crucial for many Nvidia GeForce cards to prevent the infamous Code 43 error in the Windows device manager, which indicates the driver refuses to initialize the GPU in a virtualized environment.
    Add the `<qemu:commandline>` and `<qemu:override>` sections if not already present from Phase 4.4, ensuring the `alias` matches your GPU's `<hostdev>` alias (`hostdev0`, `hostdev1`, etc. Libvirt usually assigns these automatically. You can see the alias in the `<hostdev>` tag if you save and re-edit, or just try `hostdev0` if it's the first PCI device you added this way).

    Ensure your main domain tag looks like: `<domain type='kvm' xmlns:qemu='http://libvirt.org/schemas/domain/qemu/1.0'>`

    ```xml
    <qemu:commandline>
      <!-- May include -acpitable for ssdt1.dat if used -->
      <qemu:arg value='-acpitable'/>
      <qemu:arg value='file=/path/to/your/ssdt1.dat'/>
    </qemu:commandline>

    <qemu:override>
      <qemu:device alias="hostdev0"> <!-- Ensure 'hostdev0' matches your GPU's hostdev alias -->
        <qemu:frontend>
          <!-- Use YOUR GPU's actual IDs here, converted to decimal or hex (0x prefix) -->
          <!-- Example using hex values: -->
          <qemu:property name="x-pci-vendor-id" type="unsigned" value="0x10de"/> <!-- Hex Vendor ID -->
          <qemu:property name="x-pci-device-id" type="unsigned" value="0x1c94"/> <!-- Hex Device ID -->
          <qemu:property name="x-pci-sub-vendor-id" type="unsigned" value="0x17aa"/> <!-- Hex Subsystem Vendor ID -->
          <qemu:property name="x-pci-sub-device-id" type="unsigned" value="0x3f9b"/> <!-- Hex Subsystem Device ID -->
          <!-- README-Muxless.md shows these as decimal values converted from hex. -->
          <!-- It's often easier to use hex directly if QEMU supports it, prefixing with 0x. Check QEMU documentation. -->
          <!-- Example from README-Muxless.md (decimal): -->
          <!-- <qemu:property name="x-pci-vendor-id" type="unsigned" value="4318"/> -->
          <!-- <qemu:property name="x-pci-device-id" type="unsigned" value="7316"/> -->
          <!-- <qemu:property name="x-pci-sub-vendor-id" type="unsigned" value="6058"/> -->
          <!-- <qemu:property name="x-pci-sub-device-id" type="unsigned" value="16283"/> -->
        </qemu:frontend>
      </qemu:device>
    </qemu:override>
    ```
    **Find your GPU's alias:** If you are unsure of the alias (`hostdev0`), you can check the live XML after adding the GPU: `sudo virsh dumpxml YOUR_VM_NAME`. Look for the `<hostdev>` entry for your GPU; it might have an `<alias name='hostdev0'/>` sub-tag.

*   **KVM Hiding ( `<kvm><hidden state='on'/></kvm>` ):**
    Already covered in Phase 4.4, but re-verify it's in the `<features>` section. This can help with some games or applications that detect virtualization.

*   **Hyper-V Enlightenments ( `<hyperv mode='custom'>...</hyperv>` ):**
    Also covered in Phase 4.4. Beneficial for Windows guests. Ensure the relevant enlightenments (`relaxed`, `vapic`, `spinlocks`, `vpindex`, `synic`, `stimer`, `reset`, `frequencies`) are enabled as per `win11.xml` or `README.md` suggestions.

*   **CPU Pinning and HugePages (Advanced Optimizations - Optional for now):**
    `README.md` details CPU pinning (assigning specific host CPU threads to guest vCPUs) and configuring HugePages for better memory performance. These are advanced topics. It's recommended to get basic passthrough working first, then explore these for performance tuning. We can add a dedicated section for these later if desired.

### 5.4 First Boot with GPU Passthrough & Driver Installation

1.  **Save XML Changes and Start the VM:**
    *   If using `virsh edit`, save and close.
    *   In `virt-manager`, click "Apply".
    *   Start the VM.

2.  **Check for Display Output:**
    *   Switch your monitor's input to the port connected to the passed-through GPU.
    *   You should see the VM's boot process (UEFI logo, OS loading screen) on this monitor.
    *   If you see nothing (black screen): This is the most common point of failure. Troubleshooting (Phase 7) will be needed. Common causes:
        *   VBIOS issue (wrong VBIOS, bad patch, `rom bar='off'` needed or vice-versa).
        *   Libvirt hook script failure (GPU not properly detached from host).
        *   IOMMU issues.
        *   Incorrect PCI IDs for passthrough or spoofing.

3.  **Install GPU Drivers in the Guest OS:**
    *   Once the guest OS boots using the passed-through GPU:
        *   **Windows:** Download the latest drivers for your GPU (Nvidia GeForce, AMD Radeon) from the manufacturer's website and install them. Nvidia drivers will often complain if they detect a VM without the KVM hiding and vendor ID spoofing in place, leading to Code 43.
        *   **Linux:** Install the appropriate drivers for your GPU (e.g., `nvidia`, `amdgpu`).
    *   After driver installation, reboot the VM if prompted.

4.  **Verify GPU in Guest:**
    *   **Windows:** Check Device Manager. The GPU should be listed without errors (no Code 43 for Nvidia). GPU-Z or similar tools can also verify.
    *   **Linux:** Use `lspci -nnk` inside the guest to see if the GPU is recognized and using the correct driver. Run `glxinfo | grep renderer` or a simple game/benchmark.

At this point, if you have display output and the GPU drivers are installed correctly in the guest, you have achieved basic GPU passthrough!

### 5.5 (Optional) Looking Glass Setup

If you have a muxless laptop or prefer not to switch monitor inputs, Looking Glass allows you to view and interact with the VM's GPU output in a window on your host desktop. `README-Muxless.md` and `win11.xml` detail this.

1.  **Install Looking Glass Host (Windows VM):**
    *   Download the Looking Glass host application (e.g., `looking-glass-host-Setup.exe` for B7-rc1) from [Looking Glass Downloads](https://looking-glass.io/downloads).
    *   Install it in your Windows VM.
    *   It typically installs a service that starts automatically.

2.  **Install IVSHMEM Driver (Windows VM):**
    *   The IVSHMEM device allows high-speed memory sharing between the host and guest, which Looking Glass uses.
    *   Add the IVSHMEM device to your VM's XML (as per `win11.xml`):
        ```xml
        <shmem name='looking-glass'>
          <model type='ivshmem-plain'/>
          <size unit='M'>64</size> <!-- Size can be adjusted, 32MB or 64MB often used -->
          <address type='pci' domain='0x0000' bus='0x08' slot='0x02' function='0x0'/> <!-- Libvirt assigns guest PCI slot -->
        </shmem>
        ```
        (Ensure the `<address>` bus number is not conflicting. Libvirt usually handles this.)
    *   Start the VM. Windows will detect new hardware. Point it to the IVSHMEM driver, which is often bundled with Looking Glass host releases or available separately.
    *   The `README-Muxless.md` notes downloading `ivshmem-driver-0.1.0.zip` or similar.

3.  **Install Fake Display Driver (Windows VM, if no physical display attached to dGPU):**
    *   If the dGPU in the VM has no physical monitor attached (common for muxless or headless setups), Windows might disable it or not provide a desktop to capture.
    *   Install `USBMMIDD_v2.msi` (Phase 4.1) in the Windows VM. This creates a virtual display.
    *   Configure it via Display Settings in Windows to act as your primary display at your desired resolution.

4.  **Run Looking Glass Client (Linux Host):**
    *   Compile or download the Looking Glass client for Linux.
    *   Run it from your host terminal, e.g.: `looking-glass-client -s` (Spice mouse) or other options.
    *   You may need to adjust host permissions for `/dev/shm/looking-glass`.

This concludes the main steps for OS installation and GPU passthrough. Further tweaks might involve USB device passthrough, audio configuration, and performance optimization.

---

## Phase 6: USB and Other Device Passthrough

Once your GPU is successfully passed through, you'll likely want to connect peripherals like your keyboard, mouse, game controllers, and other USB devices directly to the VM for optimal performance and compatibility. This phase covers common methods for achieving this, as well as briefly mentioning general PCI device passthrough.

### 6.1 Methods for USB Passthrough

There are several ways to give your VM control over USB devices:

#### 6.1.1 Passing Through an Entire USB Controller

This method involves dedicating an entire USB controller (and all its ports) from your host system to the VM. All devices plugged into that controller will appear directly in the VM.

1.  **Identify the USB Controller:**
    Use `lspci -nnk` on your host to list all PCI devices. Look for entries related to USB controllers (e.g., "USB controller", "XHCI Host Controller", "EHCI Host Controller"). Note their PCI addresses (e.g., `00:14.0` or `03:00.0`) and their IOMMU group (from Phase 1.2).
    ```bash
    lspci -nnk | grep -i usb
    ```

2.  **Check IOMMU Group:** Ensure the USB controller is in a separate, viable IOMMU group, or that all devices in its group can be safely passed through or are already bound to `vfio-pci`.

3.  **Bind to `vfio-pci` (if not already):**
    If the controller is not already bound to `vfio-pci` via kernel parameters (Phase 1.5), you might need to do so dynamically using your libvirt hook scripts (`vfio-startup.sh`) or by adding its PCI IDs to your `vfio-pci.ids` kernel parameter and rebooting.

4.  **Add to VM Configuration:**
    *   In `virt-manager`: Go to "Add Hardware" -> "PCI Host Device". Select the desired USB controller from the list and click "Finish".
    *   In XML (`sudo virsh edit YOUR_VM_NAME`):
        ```xml
        <hostdev mode='subsystem' type='pci' managed='yes'>
          <source>
            <address domain='0x0000' bus='0x00' slot='0x14' function='0x0'/> <!-- Use YOUR controller's PCI address -->
          </source>
          <address type='pci' domain='0x0000' bus='0x09' slot='0x00' function='0x0'/> <!-- Libvirt assigns guest PCI slot -->
        </hostdev>
        ```

*   **Pros:**
    *   All ports on the controller are passed through.
    *   Good for USB hubs connected to that controller.
    *   Often more stable for a wide range of devices.
*   **Cons:**
    *   The host system loses access to that USB controller and its ports while the VM is running.
    *   Requires a spare USB controller or careful planning.

#### 6.1.2 Passing Through Individual USB Devices (USB Host Device)

This method allows you to pass through specific USB devices based on their Vendor and Product IDs, without dedicating an entire controller.

1.  **Identify Vendor and Product ID:**
    Plug the USB device into your host. Use `lsusb` to list connected USB devices. Find your device and note its `ID VENDOR:PRODUCT` (e.g., `ID 046d:c52b` for a Logitech mouse).
    ```bash
    lsusb
    ```

2.  **Add to VM Configuration:**
    *   In `virt-manager`: Go to "Add Hardware" -> "USB Host Device". Select the desired device from the list (it will show names and IDs). Click "Finish".
    *   In XML (`sudo virsh edit YOUR_VM_NAME`):
        ```xml
        <hostdev mode='subsystem' type='usb' managed='yes'>
          <source>
            <vendor id='0x046d'/>  <!-- Hex Vendor ID (e.g., 046d) -->
            <product id='0xc52b'/> <!-- Hex Product ID (e.g., c52b) -->
          </source>
          <!-- Libvirt might add an <address type='usb' .../> tag here automatically -->
        </hostdev>
        ```

*   **Pros:**
    *   More flexible; host retains control of the USB controller.
    *   Devices can often be hotplugged (though stability varies).
*   **Cons:**
    *   Can be less stable for some devices or complex USB hubs.
    *   The VM might not see the device if it's unplugged and replugged, or if its port path changes, unless using persistent addressing methods (e.g., specific port on a hub).
    *   May require udev rules on the host to set proper permissions for QEMU/libvirt to access the device node if not running as root, or if using tools like `evdev` passthrough (see below).

#### 6.1.3 Evdev Passthrough (for Keyboard/Mouse)

`evdev` passthrough is a method to share input devices like keyboards and mice between the host and guest more seamlessly. Instead of passing through the USB device itself, you're passing the input *events* generated by the device.

1.  **Identify Device Event Path:**
    First, install `evtest` or a similar tool if you don't have it (`sudo pacman -S evtest` or `sudo apt install evtest`).
    Run `ls /dev/input/by-id/`. This directory usually contains symlinks to `eventX` devices with more descriptive names (e.g., `usb-Logitech_Gaming_Keyboard-event-kbd`). Note the full path.
    You can also run `sudo evtest` and select the device number to confirm it's the correct one by observing the event output when you use the device.

2.  **Ensure QEMU User Permissions:**
    The user QEMU runs as (often `libvirt-qemu` or `qemu`) needs read access to these `/dev/input/eventX` files. You might need to add the QEMU user to the `input` group:
    ```bash
    sudo usermod -a -G input libvirt-qemu # Or your system's QEMU user
    # You may need to restart libvirtd or reboot for group changes to take effect.
    ```
    Alternatively, udev rules can be created to set specific permissions.

3.  **Add to VM Configuration (XML only):**
    This is typically done via XML editing. Add `<input>` devices to your `<devices>` section.
    ```xml
    <devices>
      <!-- ... other devices ... -->
      <input type='passthrough' bus='virtio' source_evdev='/dev/input/by-id/usb-Logitech_USB_Keyboard-event-kbd'>
        <alias name='input0'/>
      </input>
      <input type='passthrough' bus='virtio' source_evdev='/dev/input/by-id/usb-Logitech_Gaming_Mouse_G502-event-mouse'>
        <alias name='input1'/>
      </input>
      <!-- For mouse to report as tablet for absolute positioning, might need: -->
      <!-- <input type='tablet' bus='virtio'>
        <alias name='input2'/>
      </input> -->
      <!-- The original README.md example uses virtio for bus type. -->
    </devices>
    ```
    The `README.md` implies this creates `virtio` input devices in the guest.

*   **Pros:**
    *   Allows nearly seamless switching of keyboard/mouse between host and guest (often by just moving the mouse pointer out of the VM window if using a windowed display like Looking Glass, or via a key combination).
    *   Does not require dedicating a USB controller or device.
*   **Cons:**
    *   Can be more complex to set up initially due to permissions and identifying correct event paths.
    *   Primarily for input devices (keyboard, mouse, possibly some gamepads that present as standard input devices).
    *   Special keys or features on some gaming keyboards/mice might not fully translate.

### 6.2 Passthrough for Other PCI Devices

The same method used for passing through a USB controller (PCI Host Device, Section 6.1.1) can be applied to other types of PCI devices, such as:

*   **Dedicated Network Cards:** If you want to give your VM its own physical network interface.
*   **Sound Cards:** If you have a separate PCI sound card you wish to dedicate to the VM.
*   **SATA/NVMe Controllers:** To pass through entire storage controllers (advanced).

**The key considerations remain the same:**
1.  Identify the device's PCI address and Vendor/Product IDs (`lspci -nnk`).
2.  Ensure it's in a viable IOMMU group.
3.  Bind it to `vfio-pci` (via kernel parameters or hook scripts).
4.  Add it as a `<hostdev>` PCI device in your VM's XML configuration.

### 6.3 Important Considerations

*   **IOMMU Groups:** Cannot be emphasized enough. A device can only be passed through if its IOMMU group is sound.
*   **Device Resets:** Some PCI devices (especially older ones or those not designed for virtualization) may not handle being reset by the `vfio-pci` driver or QEMU correctly. This can lead to instability or the device not working in the VM or on the host after VM shutdown. Some devices might require specific reset mechanisms or patches.
*   **Hotplugging:** While individual USB device passthrough and `evdev` can sometimes accommodate hotplugging, passing through entire PCI controllers generally means the device is attached at VM start and detached at VM stop. True PCI hotplugging in QEMU/libvirt is complex.
*   **Resource Conflicts:** Ensure the guest VM has enough PCI slots and resources allocated. Libvirt usually handles this, but complex configurations might hit limits.

This phase provides the tools to connect most necessary peripherals and other hardware directly to your virtual machine, enhancing its capabilities and user experience.

---

## Phase 7: Troubleshooting

GPU passthrough can be complex, and issues are common, especially during initial setup. This section provides guidance on diagnosing and resolving frequent problems.

### 7.1 General Troubleshooting Steps

1.  **Check System Logs (Host):**
    *   `dmesg -wH`: Watch kernel messages in real-time for errors related to `vfio-pci`, IOMMU, device binding, or your GPU drivers.
    *   `journalctl -f`: Follow the systemd journal for broader system messages.
    *   Libvirt logs: Usually found in `/var/log/libvirt/qemu/YOUR_VM_NAME.log`. This log is invaluable and often contains specific error messages from QEMU when the VM fails to start or a device fails to attach.

2.  **Check VM Logs (Guest):**
    *   **Windows:** Event Viewer (System and Application logs) can show driver errors (like Code 43) or other OS-level issues.
    *   **Linux:** `dmesg` and `journalctl` within the guest VM.

3.  **Simplify Configuration:** If you're facing many issues, temporarily revert complex XML changes (like CPU pinning, hugepages, some custom QEMU args) to a more basic setup to isolate the problem.

4.  **Double-Check IDs and Paths:** Ensure all PCI IDs, VBIOS paths, `ssdt1.dat` paths, and hook script paths in your XML and scripts are correct.

5.  **Test Components Individually:**
    *   Can the VM boot with emulated graphics? (Add back VNC/Spice display and remove GPU PCI passthrough temporarily).
    *   Do the hook scripts run correctly if manually executed (with appropriate environment variables)?

### 7.2 Common Issues and Solutions

#### 7.2.1 Black Screen on VM Start (No Display Output)

This is one of the most common problems.

*   **VBIOS Issue:**
    *   **Incorrect VBIOS:** Are you using the correct VBIOS for your card, especially if dumped or extracted? Is it properly patched (e.g., header removed for NVFlash dumps)?
    *   **`<rom file='...'/>` vs. `<rom bar='off'/>`:** Try switching between specifying your VBIOS file and using `rom bar='off'` in the GPU's `<hostdev>` XML. Some cards need one or the other. For muxless setups with VBIOS in OVMF, `rom bar='off'` is typical for the dGPU.
    *   **VBIOS not loading:** Check `dmesg` on the host and the VM's QEMU log for errors like "Failed to load ROM" or "BAR 0 is not an ISA ROM".
*   **Libvirt Hook Scripts Failure:**
    *   The GPU might not be detaching from the host. Check host `dmesg` for errors when `vfio-startup.sh` is supposed to run. Test the script manually.
    *   Ensure the hook dispatcher (`/etc/libvirt/hooks/qemu`) has the correct VM name and is executable.
*   **Driver Conflict on Host:** Ensure host GPU drivers (Nvidia, Nouveau, AMDGPU) are not interfering. Early binding to `vfio-pci` (Phase 1.5) is designed to prevent this.
*   **IOMMU/ACS Issues:** Unlikely if IOMMU groups were verified, but ensure the GPU is properly isolated.
*   **Monitor/Cable:** Double-check the monitor is on the correct input and the cable is securely connected to the passed-through GPU.
*   **UEFI/OVMF Issue:** Sometimes, the guest UEFI environment (OVMF) might not initialize the GPU correctly. Ensure you're using a compatible OVMF version. If using a custom-patched OVMF (for muxless), ensure it was built correctly and for the right GPU.

#### 7.2.2 Nvidia GPU Error Code 43 in Windows Guest

This error means the Nvidia driver has detected it's running in a VM and refuses to initialize fully.

*   **KVM Hiding Missing/Incorrect:** Ensure `<kvm><hidden state='on'/></kvm>` is in the `<features>` section of your VM XML.
*   **Vendor ID Spoofing Missing/Incorrect:** This is the primary fix. Ensure the `<qemu:override>` section with `x-pci-vendor-id`, `x-pci-device-id`, etc. is correctly configured for your GPU's `<hostdev>` entry (Phase 5.3). The IDs must match **your card's actual IDs**, and the `alias` in `<qemu:device alias="...">` must match the alias of your GPU's `<hostdev>` entry.
*   **Hyper-V Enlightenments:** Ensure appropriate Hyper-V enlightenments are enabled in the `<hyperv mode='custom'>` section. Some combinations are more effective than others. Refer to `win11.xml` or successful setups online.
*   **Fake Battery (`ssdt1.dat`):** For some GeForce cards, adding the `ssdt1.dat` via QEMU command line (`<qemu:arg value='-acpitable'/> <qemu:arg value='file=/path/to/ssdt1.dat'/>`) can help.
*   **Driver Version:** Occasionally, specific Nvidia driver versions are more problematic in VMs. Try an older or newer driver version.
*   **OVMF/Firmware:** Ensure your OVMF is up-to-date or compatible.

#### 7.2.3 VM Fails to Boot or Stuck at UEFI/BIOS Screen

*   **OVMF Issues:**
    *   **Incorrect Path:** Is the `<loader>` path to `OVMF_CODE.fd` correct in the XML? Is the `<nvram>` template path correct and is libvirt able to create a VM-specific VARS file?
    *   **Corrupt OVMF/VARS:** Try deleting the VM-specific VARS file (e.g., `/var/lib/libvirt/qemu/nvram/YOUR_VM_NAME_VARS.fd`) and letting libvirt recreate it from the template. Ensure the template itself isn't corrupt.
    *   **Secure Boot:** If using a standard OVMF, ensure Secure Boot isn't enabled by default in a way that conflicts with your unsigned drivers or OS loader if you haven't set it up.
*   **Boot Order:** Check `<boot dev='hd'/>` or `<boot dev='cdrom'/>` in the `<os>` section and the `<boot order='X'/>` for individual disk/CDROM devices.
*   **Missing VirtIO Drivers (during OS install):** If the installer can't see the disk, you forgot to load VirtIO storage drivers.
*   **Resource Allocation:** Insufficient RAM or vCPUs (though less likely to cause a boot hang at UEFI unless extremely low).

#### 7.2.4 Audio Issues (No Sound or Crackling)

*   **Passed-Through GPU Audio:** If you passed through the GPU's HDMI/DisplayPort audio device, ensure drivers are installed in the guest. Select the correct audio output device in the guest OS sound settings.
*   **Emulated Audio (ICH6/ICH9/AC97):** If relying on emulated audio:
    *   Ensure the `<sound model='ich9'>` (or similar) is in your XML.
    *   Guest drivers might be needed (usually included, but check).
    *   PulseAudio/PipeWire on the host: QEMU's audio backend might interact poorly with the host's audio server. `README.md` suggests environment variables for QEMU like `QEMU_AUDIO_DRV=pa QEMU_PA_SERVER=/run/user/1000/pulse/native` (adjust user ID). These can be set in `/etc/libvirt/qemu.conf` (`user` and `group` settings also influence this).
*   **Looking Glass Audio:** Looking Glass has its own audio streaming options; ensure they are configured if used.
*   **Crackling/Distortion:** Can be due to CPU load, incorrect audio buffer settings, or driver issues. Try increasing QEMU audio buffer sizes (via environment variables or `qemu.conf`) or ensuring CPU pinning is stable if used.

#### 7.2.5 Libvirt Hook Script Problems

*   **Permissions:** Ensure `/etc/libvirt/hooks/qemu`, `/bin/vfio-startup.sh`, and `/bin/vfio-teardown.sh` are executable (`chmod +x`).
*   **VM Name:** The VM name in the `qemu` dispatcher script must exactly match your VM's name.
*   **Script Errors:** Add `set -x` at the top of your shell scripts to enable debug output. Manually run the scripts to see where they fail.
*   **Environment:** Hook scripts run in a limited environment. Use full paths to commands.
*   **Timing:** If scripts take too long, libvirt might time out. `README.md` mentions a `KILL_TIMEOUT` in `/etc/libvirt/qemu.conf`.

#### 7.2.6 IOMMU Group / `vfio-pci` Binding Issues

*   **`dmesg` output:** Look for errors like "Failed to group device" or "vfio-pci:できない to add device".
*   **Incorrect PCI IDs for `vfio-pci.ids`:** Double-check the vendor:product IDs in your kernel command line.
*   **ACS Override Patch:** If using, ensure it's applied correctly and you understand the risks. It might not work for all motherboards/chipsets or can cause instability.
*   **BIOS Update:** Sometimes a motherboard BIOS update can improve IOMMU grouping or ACS support.

#### 7.2.7 Looking Glass Specific Issues

*   **Black Screen in Client:**
    *   Looking Glass host application not running in the VM.
    *   IVSHMEM device not configured or driver not installed in VM.
    *   Fake display driver (USBMMIDD) not installed or not set as primary in VM (if no physical display on dGPU).
    *   Firewall blocking connection (if not using local IVSHMEM).
    *   Looking Glass client version mismatch with host application version.
*   **Permissions for `/dev/shm/looking-glass` on host.**
*   **Performance:** Ensure adequate IVSHMEM size (e.g., 32MB, 64MB) and that host CPU isn't a bottleneck.

#### 7.2.8 Performance Problems (Stuttering, Low FPS)

*   **CPU Pinning:** Not configured or misconfigured. This is often crucial for smooth performance.
*   **HugePages:** Not configured or not enough available.
*   **CPU Governor:** Host CPU governor set to `powersave` instead of `performance`.
*   **Driver Issues:** Outdated or buggy drivers in host or guest.
*   **Thermal Throttling:** GPU or CPU overheating in host or guest.
*   **Insufficient Resources:** Not enough vCPUs or RAM allocated to the VM.
*   **Disk I/O Bottleneck:** Slow virtual disk performance.

This troubleshooting guide should help you tackle the most common hurdles. Remember to change one thing at a time and test systematically.

---

## Phase 8: Performance Optimizations

Once your GPU passthrough setup is stable and functional, you can explore various optimizations to enhance performance, reduce latency, and achieve a smoother experience in your virtual machine. This phase delves into common techniques like CPU pinning, HugePages, and I/O tuning.

**Important Note:** Apply these optimizations one at a time and test thoroughly. Some optimizations can have complex interactions or might not benefit all workloads or hardware configurations. Always benchmark before and after to quantify the impact.

### 8.1 CPU Pinning (vCPU to Host Core Affinity)

CPU pinning dedicates specific host CPU cores/threads to your VM's virtual CPUs (vCPUs). This prevents the host OS from scheduling other tasks on those cores, reducing jitter and cache contention, leading to more consistent VM performance, especially for latency-sensitive applications like gaming.

1.  **Identify Host CPU Topology:**
    Use `lscpu -e` or `hwloc-ls` to understand your host CPU's layout (cores, threads, NUMA nodes).
    ```bash
    lscpu -e
    # Example output:
    # CPU NODE SOCKET CORE L1d:L1i:L2:L3 ONLINE MAXMHZ    MINMHZ
    #   0    0      0    0 0:0:0:0          yes 3400.0000 800.0000
    #   1    0      0    1 1:1:1:0          yes 3400.0000 800.0000
    #   2    0      0    2 2:2:2:0          yes 3400.0000 800.0000
    #   3    0      0    3 3:3:3:0          yes 3400.0000 800.0000
    #   4    0      0    0 0:0:0:0          yes 3400.0000 800.0000 (Hyper-thread of CPU 0)
    #   5    0      0    1 1:1:1:0          yes 3400.0000 800.0000 (Hyper-thread of CPU 1)
    # ...and so on.
    ```
    It's generally recommended to pin vCPUs to physical cores first, and then their corresponding hyper-threads if needed, avoiding sharing a physical core between the host and a latency-sensitive vCPU.

2.  **Reserve Host Cores:** Decide which host cores will be dedicated to the VM. It's good practice to leave at least one or two cores (and their hyper-threads) for the host OS.

3.  **Configure CPU Pinning in XML (`sudo virsh edit YOUR_VM_NAME`):**
    Within the `<domain>` tag, add or modify the `<vcpu ...>` and `<cputune>` sections.
    ```xml
    <vcpu placement='static'>NUMBER_OF_VCPUS</vcpu>
    <cputune>
      <vcpupin vcpu='0' cpuset='HOST_CPU_CORE_FOR_VCPU0'/>
      <vcpupin vcpu='1' cpuset='HOST_CPU_CORE_FOR_VCPU1'/>
      <!-- Add more vcpupin lines for each vCPU -->
      <!-- Example: Pin 8 vCPUs to host cores 2,3,4,5 and their hyperthreads 10,11,12,13 -->
      <!-- <vcpupin vcpu='0' cpuset='2'/> -->
      <!-- <vcpupin vcpu='1' cpuset='3'/> -->
      <!-- <vcpupin vcpu='2' cpuset='4'/> -->
      <!-- <vcpupin vcpu='3' cpuset='5'/> -->
      <!-- <vcpupin vcpu='4' cpuset='10'/> -->
      <!-- <vcpupin vcpu='5' cpuset='11'/> -->
      <!-- <vcpupin vcpu='6' cpuset='12'/> -->
      <!-- <vcpupin vcpu='7' cpuset='13'/> -->
    </cputune>
    ```
    *   `placement='static'` tells libvirt that vCPUs are explicitly pinned.
    *   `cpuset` specifies the host CPU core/thread ID.
    *   Pin vCPUs to cores within the same NUMA node as the GPU if possible, especially for NUMA-aware systems.

### 8.2 Emulator and I/O Thread Pinning

Besides vCPUs, QEMU itself has emulator threads and I/O threads that can also be pinned for better performance and predictability.

1.  **Emulator Threads:** These handle general QEMU emulation tasks.
2.  **I/O Threads:** Used for disk and network I/O. If you use `threads` attribute on your `virtio-blk` or `virtio-net` devices, dedicated I/O threads are created.

**Configure in XML (`<cputune>` section):**
```xml
<cputune>
  <!-- ... vcpupin settings ... -->
  <emulatorpin cpuset='HOST_CPU_CORE_FOR_EMULATOR'/>
  <!-- Example: Pin emulator to host core 0 and its hyperthread 8 -->
  <!-- <emulatorpin cpuset='0,8'/> --> 

  <!-- If you define iothreads in your devices: -->
  <!-- <iothreadpin iothread='1' cpuset='HOST_CPU_CORE_FOR_IOTHREAD1'/> -->
  <!-- <iothreadpin iothread='2' cpuset='HOST_CPU_CORE_FOR_IOTHREAD2'/> -->
  <!-- Example: Pin iothread 1 to core 1 and its hyperthread 9 -->
  <!-- <iothreadpin iothread='1' cpuset='1,9'/> -->
</cputune>
```
*   Pin emulator and I/O threads to cores not heavily used by vCPUs, often those reserved for the host.
*   The `README.md` suggests pinning emulator threads to the host's primary core(s) and I/O threads to sibling threads of other host-reserved cores.

**Defining I/O Threads for Devices:**
To use I/O threads, you need to assign them to your devices in the XML:
```xml
<disk type='file' device='disk'>
  <driver name='qemu' type='raw' cache='none' io='native' iothread='1'/> <!-- Assigns this disk to iothread 1 -->
  <source file='/path/to/your/disk.img'/>
  <target dev='vda' bus='virtio'/>
  <address type='pci' domain='0x0000' bus='0x05' slot='0x00' function='0x0'/>
</disk>

<interface type='network'>
  <source network='default'/>
  <model type='virtio'/>
  <driver name='vhost' iothread='2'/> <!-- Assigns this NIC to iothread 2 (if supported by vhost driver) -->
  <address type='pci' domain='0x0000' bus='0x01' slot='0x00' function='0x0'/>
</interface>
```
First, you need to declare the I/O threads themselves using `<iothreads>NUMBER_OF_IOTHREADS</iothreads>` at the domain level:
```xml
<domain ...>
  <!-- ... -->
  <iothreads>2</iothreads> <!-- Example: Declares 2 I/O threads -->
  <!-- ... -->
</domain>
```

### 8.3 HugePages

HugePages are larger memory pages (typically 2MB or 1GB instead of the default 4KB) that can reduce TLB (Translation Lookaside Buffer) misses and improve memory access performance for applications that use large amounts of RAM, like VMs.

1.  **Check Host HugePages Support:**
    ```bash
    cat /proc/meminfo | grep Huge
    ```
    Look for `HugePages_Total`, `HugePages_Free`, and `Hugepagesize`.

2.  **Allocate HugePages on Host:**
    *   **Temporarily:**
        ```bash
        sudo sysctl vm.nr_hugepages=NUMBER_OF_HUGEPAGES
        # Example: For a 16GB VM with 2MB HugePages: 16 * 1024 / 2 = 8192 pages
        # sudo sysctl vm.nr_hugepages=8192
        ```
    *   **Persistently (on boot):**
        Edit `/etc/sysctl.conf` or add a file in `/etc/sysctl.d/`:
        `vm.nr_hugepages=NUMBER_OF_HUGEPAGES`
        Then run `sudo sysctl -p`.
    *   You may need to allocate them early during boot via kernel parameters if memory becomes fragmented quickly (`hugepages=NUMBER` on kernel command line).

3.  **Configure VM to Use HugePages in XML:**
    ```xml
    <memoryBacking>
      <hugepages/>
    </memoryBacking>
    ```
    Place this within the `<domain>` tag. The VM's total memory should be a multiple of the huge page size.

*   **Notes:**
    *   Allocate HugePages *before* starting the VM.
    *   If `HugePages_Free` is less than what the VM requires, the VM might fail to start or fall back to standard pages.
    *   1GB HugePages (`hugepagesz=1G hugepages=NUM_1G_PAGES` on kernel command line) can offer more benefit but require contiguous memory and BIOS/kernel support.

### 8.4 CPU Governor

Ensure your host CPU's frequency scaling governor is set to `performance` to prevent cores from throttling down, which can introduce latency.

1.  **Check Current Governor:**
    ```bash
    cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
    ```
2.  **Set to `performance`:**
    *   Install `cpupower` or similar tools (`sudo pacman -S cpupower` or `sudo apt install linux-tools-common linux-tools-$(uname -r)`).
    *   ```bash
        sudo cpupower frequency-set -g performance
        ```
    *   This might need to be set persistently via a startup service or udev rule, as it can revert on reboot or with power profile changes.

### 8.5 Other Minor Tweaks

*   **Timer Settings (`<timer>`):**
    The `README.md` mentions specific timer configurations in the `<clock>` section of the XML:
    ```xml
    <clock offset='localtime'>
      <timer name='rtc' tickpolicy='catchup'/>
      <timer name='pit' tickpolicy='delay'/>
      <timer name='hpet' present='yes'/> <!-- 'yes' or 'no' based on testing -->
      <timer name='kvmclock' present='yes'/> <!-- Often beneficial for KVM -->
      <timer name='hypervclock' present='yes'/> <!-- For Windows guests -->
    </clock>
    ```
    `hpet` can sometimes cause issues; setting `present='no'` might be necessary if you encounter problems.
*   **Disable Memory Ballooning (`<memballoon>`):**
    For performance-critical VMs where memory is statically allocated, disabling the memory balloon driver can prevent potential overhead.
    ```xml
    <memballoon model='none'/>
    ```
    If using `virtio` balloon, some prefer to keep it but just not actively use it.
*   **Disk Cache and I/O Mode:**
    For virtual disks (`<driver>` tag within `<disk>`):
    *   `cache='none'`: Good for raw images or host-cached LVM volumes. Data is written directly to disk, bypassing host page cache. Better for data integrity on crashes.
    *   `io='native'`: Uses Linux AIO (Asynchronous I/O). Can provide better performance than `io='threads'` for some workloads, especially with fast NVMe drives.
    *   `io='threads'`: Uses a QEMU thread pool for I/O. Simpler, but might not be as performant as `native`.
    Experiment with these. `cache='none' io='native'` is a common recommendation for performance with good storage.
*   **Network Driver (`<driver name='vhost'>`):**
    For `virtio-net` interfaces, using `name='vhost'` in the `<driver>` tag (e.g., `<driver name='vhost' queues='N'/>`) enables kernel-level vhost acceleration, which is generally faster than userspace QEMU handling.
    The `queues` attribute can enable multi-queue virtio-net, which can improve network throughput if the guest OS and network load can utilize it.

### 8.6 NUMA Considerations

If your host system has multiple NUMA nodes (common in multi-socket servers or some HEDT platforms):

*   **Pin VM Memory and vCPUs to a Single NUMA Node:** Try to keep all of a VM's memory and its vCPUs on the same NUMA node as the GPU being passed through. Cross-NUMA memory access incurs latency.
*   **XML Configuration for NUMA:**
    ```xml
    <cpu mode='host-passthrough' check='partial'>
      <numa>
        <cell id='0' cpus='0-7' memory='16777216' unit='KiB' memAccess='shared'/> <!-- Example: 16GB on NUMA node 0 for vCPUs 0-7 -->
      </numa>
    </cpu>
    ```
    And ensure `<memtune><node id='0' memAccess='shared'/></memtune>` or similar settings align.
    This requires careful planning based on your host's NUMA topology (`numactl -H` or `lstopo`).

Applying these optimizations requires careful testing and an understanding of your specific hardware and workload. Start with the most impactful ones like CPU pinning and HugePages, then explore others if needed.

---

## Phase 9: Security Considerations

While GPU passthrough offers near-native performance, it also introduces unique security considerations. Passing hardware directly to a virtual machine, especially one that might run untrusted software or connect to the internet, can create attack vectors if not managed carefully. This section outlines key security risks and best practices.

### 9.1 Potential Security Risks

1.  **Hardware Vulnerabilities (e.g., Spectre, Meltdown, L1TF, MDS):**
    *   CPU speculative execution vulnerabilities can potentially allow a malicious guest to infer data from the host or other guests. While mitigations exist (microcode updates, kernel patches, KVM options like ` L1TF:full,force`), direct hardware access can complicate these.
    *   **Recommendation:** Keep your host system's BIOS, CPU microcode, and kernel fully updated. Understand the KVM mitigation options for your CPU and apply them where necessary. Be aware that some mitigations can have a performance impact.

2.  **Direct Memory Access (DMA) Attacks:**
    *   PCIe devices, including GPUs, have DMA capabilities, meaning they can read/write directly to system RAM. If a guest OS is compromised, malicious software could theoretically use the passed-through GPU to attempt DMA attacks against the host's memory.
    *   **IOMMU's Role:** The IOMMU (Input/Output Memory Management Unit) is crucial here. It restricts a device's DMA access to only the memory regions assigned to the VM it's passed to. This is why ensuring your IOMMU is enabled and working correctly (Phase 1.1) is a fundamental security measure.
    *   **Recommendation:** Always ensure IOMMU is enabled and properly configured. Be cautious with devices that have known firmware vulnerabilities that could allow DMA outside IOMMU-protected regions (though rare for GPUs).

3.  **Compromised Guest OS:**
    *   If the guest OS itself is compromised (e.g., by malware), any device passed to it, including the GPU and USB devices, is effectively under the control of the attacker within the VM's context.
    *   The primary risk here is usually confined to the VM's data and any networks it's connected to. However, with DMA concerns (see above) or vulnerabilities in QEMU/KVM, escalation to the host is a theoretical possibility, albeit a complex one.
    *   **Recommendation:** Treat your guest OS like any other machine. Use firewalls, keep it updated, run anti-malware software if appropriate, and avoid running untrusted executables.

4.  **Vulnerabilities in QEMU/KVM or Libvirt:**
    *   Like any complex software, QEMU, KVM, and libvirt can have bugs. A severe vulnerability could potentially allow a compromised guest to escape the VM and gain access to the host.
    *   **Recommendation:** Keep your virtualization software (QEMU, KVM, libvirt, kernel) updated to the latest stable versions to benefit from security patches.

5.  **Libvirt Hook Scripts:**
    *   Hook scripts often run with root privileges on the host. A vulnerability in a hook script, or if the script itself is compromised, could lead to host compromise.
    *   **Recommendation:** Keep hook scripts simple, audit them carefully, and ensure their permissions are as restrictive as possible (e.g., not world-writable).

6.  **Network Exposure:**
    *   If your VM is bridged to your main network, it's as exposed as any other machine on that network. If it's compromised, it could be used to attack other devices on your LAN.
    *   **Recommendation:** Consider using a separate, isolated virtual network for the VM if it doesn't strictly need full LAN access. Use NAT or a host-only network if sufficient. If bridged, ensure the guest OS has a firewall configured.

7.  **Information Leakage via Side Channels:**
    *   Even with IOMMU, there can be subtle ways information might leak between guest and host (or other guests) through shared resources (e.g., CPU caches, memory bus contention). These are typically advanced attacks.
    *   **Recommendation:** CPU pinning can help mitigate some cache-based side channels by reducing shared core usage. Keeping mitigations for speculative execution attacks up-to-date is also important.

### 9.2 Security Best Practices

*   **Keep Host and Guest Systems Updated:** This is paramount. Apply security patches for your host OS, kernel, BIOS/firmware, QEMU/KVM/libvirt, and your guest OS and its drivers regularly.
*   **Run as Non-Root Where Possible:** While `libvirtd` often runs as root, QEMU processes for VMs can be configured to run as a less privileged user (e.g., `libvirt-qemu` or `qemu`) in `/etc/libvirt/qemu.conf`. This limits the immediate impact if QEMU itself is compromised.
*   **Use Host-Level Security Modules (AppArmor/SELinux):**
    Libvirt can integrate with AppArmor or SELinux to provide mandatory access control (MAC) for QEMU processes. This confines the VM processes, limiting what they can access on the host system even if compromised.
    Many distributions enable this by default. Check `ps auxZ | grep qemu` to see if contexts are applied.
*   **Network Segmentation:** Isolate your VM network from your main LAN if it doesn't need full access. Use libvirt's virtual networking options (NAT, isolated network) or configure host firewall rules.
*   **Minimal Guest OS:** If the VM has a dedicated purpose (e.g., just for gaming), install only necessary software to reduce its attack surface.
*   **Be Wary of Untrusted Software/Images:** Only download OS images and software from official and trusted sources.
*   **Backup Your VM and Host:** Regular backups can help you recover from a security incident or data loss.
*   **Monitor Logs:** Regularly check host and guest system logs for suspicious activity.
*   **Physical Security:** If DMA attacks are a major concern for highly sensitive environments, physical security of the machine is also relevant, as an attacker with physical access could introduce malicious PCIe devices.
*   **Consider Risks of `vfio-pci` with `unsafe_interrupts`:** If you ever needed to use the `disable_unsafe_interrupts=1` module option for `vfio-pci`, be aware of the implications, as it can make interrupt handling less secure.

Security is an ongoing process, not a one-time setup. By understanding the risks and following best practices, you can enjoy the benefits of GPU passthrough with greater confidence.

---

## Phase 10: Further Resources & Conclusion

This guide has aimed to provide a comprehensive walkthrough for setting up GPU passthrough. However, the world of virtualization and VFIO is vast and constantly evolving. This final section provides links to communities, documentation, and tools that can offer further assistance, deeper knowledge, and solutions to more niche problems.

### 10.1 Communities and Forums

*   **/r/VFIO Subreddit:** ([https://www.reddit.com/r/VFIO/](https://www.reddit.com/r/VFIO/))
    An active and invaluable community for VFIO users. You can find solutions to common problems, share success stories, ask for help, and stay updated on new developments.
*   **Level1Techs Forums:** ([https://forum.level1techs.com/](https://forum.level1techs.com/))
    Specifically, the "Linux" and "Virtualization" sections often have detailed discussions and guides related to GPU passthrough.
*   **Proxmox VE Forum:** ([https://forum.proxmox.com/](https://forum.proxmox.com/))
    While Proxmox is a hypervisor distribution, its forums contain a wealth of information on KVM/QEMU and passthrough that is often applicable even if you're not using Proxmox directly.
*   **Arch Linux Forums:** ([https://bbs.archlinux.org/](https://bbs.archlinux.org/))
    The Arch Linux community is known for its thorough documentation and knowledgeable users. Search for "VFIO" or "GPU passthrough" for relevant threads.

### 10.2 Official Documentation

*   **QEMU Documentation:** ([https://www.qemu.org/documentation/](https://www.qemu.org/documentation/))
    The official source for QEMU features and command-line options.
*   **Libvirt Documentation:** ([https://libvirt.org/docs.html](https://libvirt.org/docs.html))
    Covers libvirt's domain XML format, networking, storage management, and more.
*   **Arch Wiki - PCI passthrough via OVMF:** ([https://wiki.archlinux.org/title/PCI_passthrough_via_OVMF](https://wiki.archlinux.org/title/PCI_passthrough_via_OVMF))
    An excellent and very detailed guide that covers many aspects of GPU passthrough. Much of the information here is derived from or inspired by it and similar resources.
*   **KVM (Kernel-based Virtual Machine) Documentation:** ([https://www.linux-kvm.org/page/Documentation](https://www.linux-kvm.org/page/Documentation))
    Information about the KVM hypervisor itself.

### 10.3 Useful Tools and Scripts Repositories

*   **Looking Glass:** ([https://looking-glass.io/](https://looking-glass.io/))
    For low-latency display output on the host.
*   **Vendor-Reset (for AMD GPUs):** ([https://github.com/gnif/vendor-reset](https://github.com/gnif/vendor-reset))
    A kernel module to help with the infamous AMD GPU reset bug, which can prevent some AMD GPUs from being re-initialized properly after a VM shutdown.
*   **VFIO-Tools (various scripts):** Many users and communities share their custom hook scripts and helper tools on GitHub and GitLab. Searching for "vfio hook scripts" or similar terms can yield useful examples.

### 10.4 Conclusion

Setting up GPU passthrough is a challenging but incredibly rewarding endeavor. It unlocks the ability to run demanding graphical applications, games, or entire operating systems with near-native GPU performance within a virtual machine, all while retaining the flexibility and power of your host Linux environment.

This guide has attempted to consolidate information from various sources, including the original `README.md` and `README-Muxless.md` from this repository, along with common practices from the VFIO community, to provide a step-by-step path for both muxed and muxless GPU setups. The goal was to cover host preparation, VBIOS handling, libvirt hooks, VM creation, OS installation, GPU passthrough itself, USB/device passthrough, troubleshooting, performance optimization, and security considerations.

Remember that every system can be slightly different, and new hardware or software versions might introduce new quirks. Patience, careful log checking, and community resources are your best allies. We hope this guide serves as a solid foundation for your GPU passthrough journey.

Good luck, and enjoy your high-performance virtual machines!

---
