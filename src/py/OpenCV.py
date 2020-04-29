import cv2

myrtmp_addr = "rtmp://192.168.100.24:1935/live/demo"
# cap = cv2.VideoCapture(myrtmp_addr)
# frames,err = cap.read()

cap = cv2.VideoCapture(myrtmp_addr)

while True:
    try:
        ret, frame = cap.read()
        cv2.imshow('Captured frame', frame)
        if cv2.waitKey(1) == 13: #13 is the Enter Key
            break
    except: pass

# Release camera and close windows
cap.release()
cv2.destroyAllWindows()
