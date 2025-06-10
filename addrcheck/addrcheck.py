binFileName = '..\\traces\\2025-05-18-v3-boot-2-leds-RST-PHI2-CS1-2-A0-A11.bin'

RST = 0
PHI2 = 1
_CS1 = 2
_CS2 = 3
A0 = 4
A11 = 15


binFile = open(binFileName, 'rb')

pastReset = False
prevClockHigh = False   
clockRising = False

binDataRemaining = True
sampleNum = 0
while binDataRemaining and sampleNum < 200200:
    binChunk = binFile.read(1024)
    if not binChunk:
        binDataRemaining = False
    else:
        for i in range(0, len(binChunk) - 1, 2):
            firstByte = binChunk[i]
            secondByte = binChunk[i+1]
            sample = (secondByte << 8) | firstByte

            if not pastReset and (sample & (1 << RST)) == (1 << RST):
                pastReset = True
                print(f"RST: {sampleNum}")

            clockHigh = (sample & (1 << PHI2)) == (1 << PHI2)
            if prevClockHigh == False:
                if clockHigh:
                    clockRising = True
                    clockRisingSampleNum = sampleNum
                    # print(f"Phi2 Edge: {sampleNum}")
                else:
                    clockRising = False

            sampleNum += 1
            prevClockHigh = clockHigh              

            if pastReset and sampleNum == clockRisingSampleNum + 2:
                clockRisingSampleNum = 0

                cs1 = not ((sample & (1 << _CS1)) == (1 << _CS1))
                cs2 = (sample & (1 << _CS2)) == (1 << _CS2)

                if cs1 and cs2:
                    address = sample >> 4
                    print(f"F{address:03X}")

binFile.close()