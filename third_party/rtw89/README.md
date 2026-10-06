# rtw89 (not modified)

These files are copied without any change from Linux:

- Repository: https://github.com/torvalds/linux.git
- Version: tag v7.2, commit 8d3ae59288f1e7d58d76558a6ee96d533bc5019f
- Path: `drivers/net/wireless/realtek/rtw89/`

They are the 26 `.c` files that `buildhal.ps1` compiles (the rtw89_core, rtw89_pci, rtw89_8852b_common,
rtw89_8852b and rtw89_8852be modules of the Makefile, without wow.c and debug.c) and the 23 `.h` files they
include.

Licence: every file has `SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause`. This project uses them under
BSD-3-Clause. Both licence texts are in `LICENSES/` (`LICENSES/preferred/BSD-3-Clause` and
`LICENSES/preferred/GPL-2.0` of the same Linux commit). Copyright (c) 2019-2026 Realtek Corporation; see the
header of each file.

To check a file against the upstream commit, compare `git hash-object <file>` with its blob id below.

| File | Bytes | Git blob id |
|---|---:|---|
| acpi.c | 33082 | ebb6cf5557473a7672c942deda7f4c5d2b11c0d6 |
| acpi.h | 7476 | 48a46f2005b199cecd082f3d77ec5461e7dd5e99 |
| cam.c | 40441 | 7c62ece137d01da835fba1f77ae04e9bb86bb077 |
| cam.h | 17813 | 22868f2622430fcec4d34166edddcabb7cfec8b7 |
| chan.c | 93092 | 6f11335b4968749507a354498b0a798adedc854d |
| chan.h | 6816 | a9a5f1b307a2785757874464f8190cd857ddc238 |
| coex.c | 362000 | 0f7ae572ef91510d169f2176a04174ba535c34fe |
| coex.h | 9956 | ea2c1e5d70f51841dac8c19c3412f0f6405ec46e |
| core.c | 227596 | 68dad6090f87e22cc996beffa601b32847716951 |
| core.h | 204305 | 5547888d7e67a40c5eb9a98a5db84996e5135ebc |
| debug.h | 2678 | 7cdceb24f52dd8d839728dc7d81065201cdf481e |
| efuse_be.c | 13908 | 70c1b8be662e14e9a7c42ffcd58b22ac85e5fa54 |
| efuse.c | 13234 | 89d4b1b865f8ff6f6a9686e636ef51733740ff7f |
| efuse.h | 1182 | a14a9dfed8e8f23f8b408f807f759741c7aa7a56 |
| fw.c | 336376 | d6a594b75ab20232b27f9b1a6c5f07cfa6c869ff |
| fw.h | 177557 | 20721d5209aa3a1149ebe691bd41e008781cedfd |
| mac_be.c | 100211 | f24c119b99f1bd0b61b7168e4a7c7720e810c7c3 |
| mac.c | 223875 | 8c395517bd2fe1c9af5eb0b26eccfadcb0e84e19 |
| mac.h | 58161 | 539061fc15e85bac6173d2137c3fce7e1ee244cd |
| mac80211.c | 55054 | aade5c5b79e82dffa353bc612a6960b3f71eae6b |
| pci_be.c | 27909 | 6390980b8ee060bc1b4c6a14ff69328c6093a6d4 |
| pci.c | 135190 | 102bae4881805b467a44111dee8866580b1b1f48 |
| pci.h | 60590 | c3f2d0df5846d876b944be65cca9ffa3d998935f |
| phy_be.c | 75531 | 99263355e2f158127056c15c5b19c9db1de2a571 |
| phy.c | 264247 | 759be4dab42b1c21c10f414226e83a40408e8eb0 |
| phy.h | 37448 | 532232892831f0517c51630369a840aaf0de5e39 |
| ps.c | 15573 | 31bb5dcd284aeca6518e4595ccad04718b2c9907 |
| ps.h | 1880 | 729477153de66bf71be6bbcdd0832053e288c148 |
| reg.h | 405719 | 086ef77ebb9fcdcf6dcbd3614dca4711f0703d64 |
| regd.c | 39639 | 14fab6b1a74a7588ca34979d58397ceacf8023df |
| rtw8852b_common.c | 72698 | df5fbae50ff5507e728042e078e20ace2f2f0af2 |
| rtw8852b_common.h | 10476 | 3dce5422f41e570c4e53d7c373762dd04e355853 |
| rtw8852b_rfk_table.c | 32539 | 0b8a210bb10b72f9549347ccbaa1ac25c7866462 |
| rtw8852b_rfk_table.h | 3735 | b4d6e9851ff99555882e21f668db3cae7435f962 |
| rtw8852b_rfk.c | 134756 | 5cfacc10e7c8e26907904ba4758d12afd32aa5fd |
| rtw8852b_rfk.h | 1473 | 5fae980d5e2c240aa8c0b2c81dedaf139d712bfc |
| rtw8852b_table.c | 748750 | 07945d06dc59c9cf0f0964093560298e24004b82 |
| rtw8852b_table.h | 647 | da6c90e2ba9314446dbd0cca6a57a9fa14ba6bf4 |
| rtw8852b.c | 37587 | 4e7b068aaa75b1d4d7908039cc337a8b0ae0cc3c |
| rtw8852b.h | 300 | 5ec7180fd355003b43e4e6404a3a19ba82db27ec |
| rtw8852be.c | 3229 | 5bc0a6a99d1d2b352c43044c80ad11d0c1ba076c |
| sar.c | 24305 | 6fe3822eb598701da2434478cd7f78c138e83f87 |
| sar.h | 1359 | 60b3aec5b3be1ffe27c62220f5b01596954bdc4f |
| ser.c | 20934 | a507ce1fcd6307b3af56798e6b601ad2792dc5b1 |
| ser.h | 387 | 6b8e62019942db5bf9f268321599b9ad627930c2 |
| txrx.h | 30896 | 4e7109fbe7c9919bea077a5b208c476c380c75a3 |
| util.c | 8871 | 073714db26f240a97d2e78ff10538819d03866c6 |
| util.h | 2869 | c16e7a7f8bc922f3de3e45e45c03876b0195dd7d |
| wow.h | 3485 | d7e67632efebbcd25435de6f127d3c967a6dfe4c |

49 files, 4187835 bytes.
