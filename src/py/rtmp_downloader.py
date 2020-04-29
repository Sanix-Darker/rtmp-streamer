# -*- coding: utf-8 -*-
import os
from ctypes import cdll, c_char_p, c_int, create_string_buffer, sizeof

class RTMP:
    """
    This class encapsulates librtmp in order to connect to a RTMP server and get video stream.
    RTMPDUMP website : http://rtmpdump.mplayerhq.hu
    LIBRTMP manpage  : http://rtmpdump.mplayerhq.hu/librtmp.3.html
    """

    LogLevel = {
        "RTMP_LOGCRIT"      : c_int(0),
        "RTMP_LOGERROR"     : c_int(1),
        "RTMP_LOGWARNING"   : c_int(2),
        "RTMP_LOGINFO"      : c_int(3),
        "RTMP_LOGDEBUG"     : c_int(4),
        "RTMP_LOGDEBUG2"    : c_int(5),
        "RTMP_LOGALL"       : c_int(6)
    }

    def __init__(self, tcURL, output_file_path, log_level=LogLevel["RTMP_LOGINFO"], buffer_size=64 * 1024):
        # Load library
        if os.name == 'nt':
            self.lib = cdll.LoadLibrary('lib/librtmp.dll')
        elif os.name == 'posix':
            self.lib = cdll.LoadLibrary('librtmp.so.0')

        # Initialize variables
        self.tcURL              = tcURL
        self.output_file_path   = output_file_path
        self.log_level          = log_level
        self.buffer_size        = buffer_size

    #************************************************************************
    # Wrap native library functions

    def Alloc(self):
        """Return a pointer to a new RTMP object"""
        return self.lib.RTMP_Alloc()

    def Init(self, r_pointer):
        """Init the RTMP object"""
        self.lib.RTMP_Init(r_pointer)
        return None

    def SetupURL(self, r_pointer, tcURL):
        """
        Setup the rtmp url
        The rtmp url format is of the form
        rtmp[t][e|s]://hostname[:port][/app[/playpath]]
        """
        self.lib.RTMP_SetupURL(r_pointer, c_char_p(tcURL))
        return None

    def Connect(self, r_pointer):
        """Established network connection"""
        self.lib.RTMP_Connect(r_pointer, None)
        return None

    def ConnectStream(self, r_pointer):
        """Established RTMP session"""
        self.lib.RTMP_ConnectStream(r_pointer, 0)
        return None

    def Read(self, r_pointer, buffer):
        """
        Reads bytes from the stream an write its into the buffer.
        Returns the number of bytes read
        When it returns 0 bytes, the stream is complete and may be closed
        """
        return self.lib.RTMP_Read(r_pointer, buffer, sizeof(buffer))

    def Close(self, r_pointer):
        """ Closes the connection """
        self.lib.RTMP_Close(r_pointer)
        return None

    def Free(self, r_pointer):
        """ Frees the session """
        self.lib.RTMP_Free(r_pointer)
        return None

    def LogSetLevel(self, log_level):
        """ Defines RTMP_LogLevel used by output """
        self.lib.RTMP_LogSetLevel(log_level)
        return None

    #************************************************************************

    def getVideoStream(self):
        """
        Get the video stream from the rtmp server
        """
        # Try to open the output file
        try:
            output_file = open(self.output_file_path, 'wb')
        except:
            print ("Cannot open output file.\n Please check the output_file_path\n. Aborting.")
            return None

        # Instantiate buffer
        buffer = create_string_buffer(self.buffer_size)

        # Set LogLevel
        self.LogSetLevel(self.log_level)

        # Setup RTMP connection
        r_pointer = self.Alloc()
        self.Init(r_pointer)
        self.SetupURL(r_pointer, self.tcURL)

        # Connect and establish session
        self.Connect(r_pointer)
        self.ConnectStream(r_pointer)

        # Read stream
        try:
            while True:
                # While result > 0, write buffer bytes into output_file
                result = self.Read(r_pointer, buffer)
                output_file.write(buffer[:result])
                if result == 0:
                    break
        except:
            # Handles exception in order to close session and file properly
            print ("An exception occured. Ending session.")

        # Ends session and closes output_file
        self.Close(r_pointer)
        self.Free(r_pointer)
        output_file.close()



# construct the url before as mentionned in librtmp manpage
m_rtmp = RTMP('rtmp://82.192.84.30:1935/live/myStream.sdp', 'out_file.flv', RTMP.LogLevel["RTMP_LOGDEBUG"] )
m_rtmp.getVideoStream()