fileName = "2025-09-18-v1-boot+format-garbage-RST-PHI2-RW-SYNC-D0-D7-IEEE-writes.txt"

file = open(fileName, "r")

for line in file:
    byteStr = line[13:15]
    if byteStr == "FF":
        continue

    byteNeg = int(byteStr, 16)
    byte = byteNeg ^ 0xFF
    # print(hex(byteNeg), " ", end="")
    char = chr(byte)
    if byte > 31 and byte < 127:
        print(f"{byte:02x}" + "-" + char + " ", end="")
    else:
        print(f"{byte:02x}" + " ", end="")


file.close()