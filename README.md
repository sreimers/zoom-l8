# Requirements - Arch Linux
```bash
$ sudo pacman -S git make gcc linux-headers dkms
```

# Requirements - Ubuntu

```bash
$ sudo apt-get install build-essential dkms git
```


# Installation

```bash
$ sudo git clone https://github.com/sreimers/zoom-l8.git /usr/src/snd-usb-zoom-0.0.1
$ sudo dkms add snd-usb-zoom/0.0.1
$ sudo dkms autoinstall
```

# Update

TBD


# Remove

```bash
$ sudo dkms remove snd-usb-zoom/0.0.1
```


# Notes

## Input (12 CH)

- Master L
- Master R
- In1 
- In2
- In3
- In4
- In5
- In6
- In7L
- In7R
- In8L 
- In8R

## Output (4 CH)

- Out1 (USB 1)
- Out2 (USB 2)
- Out3 (USB 3)
- Out4 (USB 4)


## USB format:

URB = 512 Byte
1 CH = 4 Byte (32 Bit)

(ML)(MR)(In1)(In2)... (128 Byte) (32 possible Channels, L-8 uses 12 CH, see Input)
(ML)(MR)(In1)(In2)... (128 Byte)
(ML)(MR)(In1)(In2)... (128 Byte)
(ML)(MR)(In1)(In2)... (128 Byte)

1 URB = 1/48000 * 4 Samples = 83us/URB
