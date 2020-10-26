#include <Arduino.h>
#include "WifiInboundHandler.h"
#include "RingStream.h"
#include "CommandDistributor.h"
#include "DIAG.h"

WifiInboundHandler * WifiInboundHandler::singleton;

void WifiInboundHandler::setup(Stream * ESStream) {
  singleton=new WifiInboundHandler(ESStream);
}

void WifiInboundHandler::loop() {
  singleton->loop1();
}


WifiInboundHandler::WifiInboundHandler(Stream * ESStream) {
  wifiStream=ESStream;
  clientPendingCIPSEND=-1;
  inboundRing=new RingStream(INBOUND_RING);
  outboundRing=new RingStream(OUTBOUND_RING);
  pendingCipsend=false;
} 


// Handle any inbound transmission
// +IPD,x,lll:data is stored in streamer[x]
// Other input returns  
void WifiInboundHandler::loop1() {
   // First handle all inbound traffic events because they will block the sending 
   if (loop2()!=INBOUND_IDLE) return;

    // if nothing is already CIPSEND pending, we can CIPSEND one reply
    if (clientPendingCIPSEND<0) {
       int next=outboundRing->read();
       if (next>=0) {
         currentReplySize=outboundRing->count();
         if (currentReplySize==0) {
          outboundRing->read(); // drop end marker
         }
         else {
          clientPendingCIPSEND=next-'0'; // convert back to int         
          pendingCipsend=true;
         }
       }
    } 

    if (pendingCipsend) {
         if (Diag::WIFI) DIAG( F("\nWiFi: [[CIPSEND=%d,%d]]"), clientPendingCIPSEND, currentReplySize);
         StringFormatter::send(wifiStream, F("AT+CIPSEND=%d,%d\r\n"),  clientPendingCIPSEND, currentReplySize);
         pendingCipsend=false;
         return;
      }
    
    
    // if something waiting to execute, we can call it 
      int next=inboundRing->read();
      if (next>0) {
         int clientId=next-'0'; //convert char to int
         int count=inboundRing->count();
         if (Diag::WIFI) DIAG(F("\nExec waiting %d %d:"),clientId,count); 
         byte cmd[count+1];
         for (int i=0;i<count;i++) cmd[i]=inboundRing->read();   
         cmd[count]=0;
         if (Diag::WIFI) DIAG(F("%e\n"),cmd); 
         
         outboundRing->mark();  // remember start of outbound data 
         outboundRing->print(clientId);
         CommandDistributor::parse(clientId,cmd,outboundRing);
         // The commit call will either write the null byte at the end of the output,
         // OR rollback to the mark because the commend generated more than fits rthe buffer 
         outboundRing->commit();
         return;
      }
   }



// This is a Finite State Automation (FSA) handling the inbound bytes from an ES AT command processor    

WifiInboundHandler::INBOUND_STATE WifiInboundHandler::loop2() {
  while (wifiStream->available()) {
    int ch = wifiStream->read();

    // echo the char to the diagnostic stream in escaped format
    if (Diag::WIFI) {
      // DIAG(F(" %d/"), loopState);
      StringFormatter::printEscape(ch); // DIAG in disguise
    }

    switch (loopState) {
      case ANYTHING:  // looking for +IPD, > , busy ,  n,CONNECTED, n,CLOSED 
        
        if (ch == '+') {
          loopState = IPD;
          break; 
        }
        
        if (ch=='>') { 
           for (int i=0;i<currentReplySize;i++) {
             int cout=outboundRing->read();
             wifiStream->write(cout);
             if (Diag::WIFI) StringFormatter::printEscape(cout); // DIAG in disguise
           }
           outboundRing->read(); // drop the end marker
           clientPendingCIPSEND=-1;
           pendingCipsend=false;
           loopState=SKIPTOEND;
           break;
        }
        
        if (ch=='R') { // Received ... bytes 
          loopState=SKIPTOEND;
          break;
        }
        
        if (ch=='b') {   // This is a busy indicator... probabaly must restart a CIPSEND  
           pendingCipsend=(clientPendingCIPSEND>=0);
           loopState=SKIPTOEND; 
           break; 
        }
        
        if (ch>='0' && ch<='9') { 
              runningClientId=ch-'0';
              loopState=GOT_CLIENT_ID;
              break;
        }
        
        break;
        
      case IPD:  // Looking for I   in +IPD
        loopState = (ch == 'I') ? IPD1 : SKIPTOEND;
        break;
        
      case IPD1:  // Looking for P   in +IPD
        loopState = (ch == 'P') ? IPD2 : SKIPTOEND;
        break;
        
      case IPD2:  // Looking for D   in +IPD
        loopState = (ch == 'D') ?  IPD3 : SKIPTOEND;
        break;
        
      case IPD3:  // Looking for ,   After +IPD
        loopState = (ch == ',') ? IPD4_CLIENT : SKIPTOEND;
        break;
        
      case IPD4_CLIENT:  // reading connection id
        if (ch >= '0' || ch <='9'){
           runningClientId=ch-'0';
           loopState=IPD5;
        }
        else loopState=SKIPTOEND;
        break;
        
      case IPD5:  // Looking for ,   After +IPD,client
        loopState = (ch == ',') ? IPD6_LENGTH : SKIPTOEND;
        dataLength=0;  // ready to start collecting the length
        break;
        
      case IPD6_LENGTH: // reading for length
        if (ch == ':') {
          if (dataLength==0) {
            loopState=ANYTHING;
            break;
          }
          if (Diag::WIFI) DIAG(F("\nWifi inbound data(%d:%d):"),runningClientId,dataLength); 
          if (inboundRing->freeSpace()<=(dataLength+1)) {
            // This input would overflow the inbound ring, ignore it  
            loopState=IPD_IGNORE_DATA;
            break;
          }
          inboundRing->mark();
          inboundRing->print(runningClientId); // prefix inbound with client id
          loopState=IPD_DATA;
          break; 
        }
        dataLength = dataLength * 10 + (ch - '0');
        break;
        
      case IPD_DATA: // reading data
         inboundRing->write(ch);    
        dataLength--;
        if (dataLength == 0) {
           inboundRing->commit();    
          loopState = ANYTHING;
        }
        break;

      case IPD_IGNORE_DATA: // ignoring data that would not fit in inbound ring
        dataLength--;
        if (dataLength == 0) loopState = ANYTHING;
        break;

      case GOT_CLIENT_ID:  // got x before CLOSE or CONNECTED
        loopState=(ch==',') ? GOT_CLIENT_ID2: SKIPTOEND;
        break;
        
      case GOT_CLIENT_ID2:  // got "x,"  before CLOSE or CONNECTED
        loopState=(ch=='C') ? GOT_CLIENT_ID3: SKIPTOEND;
        break;
        
      case GOT_CLIENT_ID3:  // got "x C" before CLOSE or CONNECTED (which is ignored)
         if(ch=='L') {
          // CLOSE 
          if (runningClientId==clientPendingCIPSEND) {
            // clear the outbound for this client
            for (int i=0;i<=currentReplySize;i++) outboundRing->read(); 
          }
         }
         loopState=SKIPTOEND;   
         break;
         
      case SKIPTOEND: // skipping for /n
        if (ch=='\n') loopState=ANYTHING;
        break;
    }  // switch
  } // available
  return (loopState==ANYTHING) ? INBOUND_IDLE: INBOUND_BUSY;
}
