# shredos_hexviewer
A hexadecimal / ascii viewer for ShredOS with a focus on confirming disc erasure at a technical level.

**`shredos_hexviewer`** is a high-performance, ncurses-based terminal disk inspection utility designed specifically for modern 64-bit Linux environments and integrated into the **ShredOS** live data sanitization distribution.

It allows system administrators, data sanitization technicians, and security auditors to visually inspect raw block storage devices (NVMe, SATA/SAS, SCSI, USB drives), perform ultra-fast pattern searches, verify erasure completeness, and analyze drive entropy levels to detect lingering data or verify NIST-compliant sanitization.

## Key Features

* **Dynamic Resizable Layout:** Automatically scales row widths, hex fields, ASCII panels, and status bars to fill your terminal window cleanly.
* **Auto-Discovery & Hot-Plug Polling:** Dynamically detects block devices via `/sys/block`, retrieves hardware models, serial numbers, and auto-detects partition signatures (GPT, MBR, NTFS, ext2/3/4, FAT, ISO9660).
* **Multi-Unit Scope Support:** Specify search and scan ranges seamlessly using percentage (`%`), megabytes (`MB`), gigabytes (`GB`), or terabytes (`TB`).
* **1000-Chunk Scatter Mode:** Perform ultra-fast pseudo-random statistical sampling across large multi-terabyte drives in seconds without waiting for hours of sequential scanning.
* **Heuristic Analysis Engine:** Built-in Shannon Entropy calculators and a natural language text detection engine to discover structured files, hidden partitions, or multi-word phrases.
* **Root Privileges Protection:** Ensures direct, low-level block storage read permissions before launch.

## Building shredos_hexviewer

#### Debian/Ubuntu
```
sudo apt install \
  build-essential \
  pkg-config \
  automake \
  libncurses-dev
```
#### Fedora / RHEL / CentOS
```
sudo bash
sudo dnf update -y
sudo dnf groupinstall -y "Development Tools" "C Development Tools and Libraries"
sudo dnf install -y \
  ncurses-devel
```
#### then
```
./autogen.sh
./configure
make
```
## Main Screen
<img width="994" height="434" alt="Screenshot_20261018_214843" src="https://github.com/user-attachments/assets/bbea85e6-5660-4e14-b7f9-b803a587fe00" />


## Application Layout & Navigation
The interface is divided into three primary regions:
1. Left Pane (Devices): Lists all detected storage devices with model and serial numbers. Use Up/Down keys to switch active drives instantly.
2. Right Pane (Hex View / Help): Displays raw hex addresses, hexadecimal byte representations, printable ASCII characters, and a dynamic bottom status bar indicating exact LBA 512-byte block boundaries and ranges.
3. Bottom Bar: Quick action bar showing accessible hotkeys.

### Navigation Hotkeys
|Key | Description |
| -- | ----------- |
| Up / Down | Select drive in left |
| panePgUp / PgDnScroll Hex View | forward or backward by one full page |
| < / > | Jump directly to the start (0x0) or end of the disk |
| g / G | Open Go To popup (Percentage, Byte Offset, or LBA Block Number) |
| s / S | Open Search Category menu |
| e / E | Open Analysis & Heuristics menu |
| r / R | Refresh / Reload system drive list |
| h / H | Toggle on-screen Help & Reference page |
| q / Q | Quit application

## Search Functionality (s)
<img width="994" height="434" alt="Screenshot_20261018_215014" src="https://github.com/user-attachments/assets/045b7af7-4c16-4469-ae24-1502ab0d7810" />


The search engine allows scanning raw disk sectors using both targeted patterns and flexible sampling scopes.

### Search Categories
- Non-Zero Search (!= 0x00): Rapidly scans the drive for any byte that is not zero (0x00). Ideal for confirming zero-pass erasures.
- Non-0xFF Search (!= 0xFF): Scans for any byte that is not 0xFF. Useful for inspecting flash memory states (e.g., erased SSD blocks).
- Hex Pattern Search: Supports raw byte sequences and ?? wildcards (e.g., 4D 5A ?? 00 to find PE headers regardless of flags).
- ASCII Pattern Search (Case-Sensitive): Scans for exact string matches across disk blocks.
- ASCII Pattern Search (Case-Insensitive): Scans for text strings regardless of character casing.

### Continuation (n)
When a match is located, pressing n ("Next") instantly continues the search forward from the current hit position without resetting parameters.

### Search Scopes (Sequential vs. Scatter)
When prompted for a search scope:
- 100% (Full Sequential Scan): Reads every sector sequentially from start to target.
- <100% (1000-Chunk Scatter Scan): Divides the target range into 1,000 equal segments and scans them in pseudo-random order. This allows sampling terabytes of drive space in seconds to detect residual data across the disk surface.
- Custom Units: Supports ranges like 50%, 500M, 8GB, or 2TB.

## Goto - by disc percentage, byte offset or block (LBA)
<img width="994" height="434" alt="Screenshot_20261018_215111" src="https://github.com/user-attachments/assets/3b4035b4-9104-4a01-a546-1855dc7632b6" />


## Entropy Theory & Advanced Checks (e)

**What is Shannon Entropy?**
Information entropy measures the degree of randomness or unpredictability in a set of data. shredos_hexviewer calculates Shannon Entropy ($H$) using the standard formula:

<img width="304" height="92" alt="Screenshot_20261018_205619" src="https://github.com/user-attachments/assets/0705cfe0-1f83-4dce-a3f9-22e582137b31" />

<img width="994" height="434" alt="Screenshot_20261018_215529" src="https://github.com/user-attachments/assets/5eff9456-5193-490c-8824-859f1cd30a53" />


### What the Shannon score means:
Represents the probability (relative frequency) of byte value occurring within the sampled block or drive scope.
- **Min Entropy (0.0):** Uniform, completely predictable data (e.g., a sector filled entirely with 0x00 or 0xFF) or unformatted empty space.
- **Low Entropy (1.0 - 4.0):** Repeating byte patterns, raw text, or sparse file tables.
- **Medium Entropy (5.0 - 6.9):** Executables, structured files, and uncompressed documents. (e.g 6.2788 score—typical of an active partition containing standard files, filesystem tables, or application binaries).
- **Max Entropy (7.9 - 8.0):** High-entropy random data, compressed archives, encrypted sectors, or NIST 800-88 compliant random wipe verification.

### Analysis Modes
- **Global Shannon Entropy Check:** Calculates the aggregate Shannon Entropy across the selected scope (100% sequential or 1000-chunk scatter). Provides a single macro score ($0.0 - 8.0$) to give an immediate statistical profile of the drive state.
- **Shannon Entropy Outlier Search (Block-by-Block):** Scans a drive block-by-block (4KB blocks) looking for regions where the entropy drops below a user-defined threshold (default: < 5.0).Use Case: When auditing a drive that has been encrypted or filled with random data, this mode quickly pinpoints "outliers"—unencrypted file fragments, plain-text boot sectors, or zero-filled blocks. Supports n for finding subsequent outliers.
- **Multi-Word Phrase Search (Natural Language Engine):** Uses a heuristic parsing engine to locate structured human language text across raw sectors without needing a intact filesystem.Logic: Evaluates word structures, ignoring internal quotes (' and "). Requires words to meet letter/vowels ratios (max 3 consecutive consonants, mandatory vowels for 3+ letter words) and verifies average word lengths.Use Case: Recovers or verifies plain-text documents, configuration files, and log remnants across unallocated space. Supports n for finding next phrase matches.
- **Single Block Shannon Entropy Check:** Inspects a specific 512-byte LBA block index or exact byte offset and returns the instant entropy score and status classification (e.g., Plain Zeros, Structured File, Max Random Noise).

⚠️ **Critical Caution on Entropy Scans:**
Shannon entropy (whether global or block-by-block) measures statistical randomness, not semantics. An entropy scan will not flag a small snippet of plain text (such as "How now brown cow") if it resides inside a 512-byte block alongside random or high-entropy data. Because the surrounding bytes maintain a high-entropy distribution, the block's overall score remains high, masking the text. To locate human-readable strings hidden amongst random or wiped data, always use the Multi-Word Phrase Search or ASCII Pattern Search instead.

<img width="994" height="434" alt="Screenshot_20261018_215722" src="https://github.com/user-attachments/assets/834f5ec2-0eed-454e-8e44-6c24f68fb696" />


## Partition and Filesystem Signature Detection
When shredos_hexviewer initializes and discovers storage devices, it performs an automatic scan of the initial sectors to identify partition tables, boot records, and filesystems. This detected signature is dynamically displayed in the top border of the hex view pane (for example, [GPT], [MBR + NTFS], or [ext2/3/4]).

The underlying signature detection mechanism relies on specific low-level byte patterns:

- GPT (GUID Partition Table): Checked by reading LBA 1 (byte offset 512) and scanning for the explicit header string "EFI PART".

- MBR (Master Boot Record): Verified by locating the standard boot signature bytes (0x55AA) located at offsets 510 and 511 of the first sector.

- Filesystem & Hybrid Signatures: When an MBR is detected, the program can combine indicators (such as MBR + NTFS or MBR + ext2/3/4). It checks for specific magic bytes and file system markers:

  - NTFS: Looks for the "NTFS    " signature string starting at offset 3.

  - ext2 / ext3 / ext4: Verifies the 0xEF53 magic number at byte offset 1080.

  - FAT / FAT32: Looks for standard FAT volume label fields and structure parameters.

  - ISO9660: Performs a targeted read at byte offset 32,768 (the 32KB mark) to locate optical disc "CD001" formatting.

  - Raw / Unknown: If no known partition table, boot sector signature, or filesystem magic header is matched, it defaults to labeling the storage as Raw or Raw / Unknown.
<img width="994" height="434" alt="Screenshot_20261018_215916" src="https://github.com/user-attachments/assets/489494ab-bf19-4178-97a7-3d0335073fba" />





