//////////////////////////////////////////////////////////////////////////
//                                                                      //
// XrdConnMgr                                                           //
// Author: Fabrizio Furano (INFN Padova, 2004)                          //
// Adapted from TXNetFile (root.cern.ch) originally done by             //
//  Alvise Dorigo, Fabrizio Furano                                      //
//          INFN Padova, 2003                                           //
//                                                                      //
// The Connection Manager handles socket communications for TXNetFile   //
// action: connect, disconnect, read, write. It is a static object of   //
// the TXNetFile class such that within a single application multiple   //
// TXNetFile objects share the same connection manager.                 //
// The connection manager maps multiple logical connections on a single //
// physical connection.                                                 //
// There is one and only one logical connection per client              //
// and one and only one physical connection per server:port.            //
// Thus multiple objects withing a given application share              //
// the same physical TCP channel to communicate with the server.        //
// This reduces the time overhead for socket creation and reduces also  //
// the server load due to handling many sockets.                        //
//                                                                      //
//////////////////////////////////////////////////////////////////////////

#include "XrdConnMgr.hh"
#include "XrdDebug.hh"
#include "XrdMessage.hh"
#include "XrdMutexLocker.hh"

#ifdef AIX
#include <sys/sem.h>
#else
#include <semaphore.h>
#endif

XrdConnectionMgr *XrdConnectionMgr::fgInstance = 0;

//_____________________________________________________________________________
extern "C" void * GarbageCollectorThread(void * arg)
{
   // Function executed in the garbage collector thread

   int i;
   XrdConnectionMgr *thisObj = (XrdConnectionMgr *)arg;

   pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, 0);
   pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, 0);

   while (1) {
      pthread_testcancel();

      thisObj->GarbageCollect();

      for (i = 0; i < 10; i++) {
	 pthread_testcancel();

         usleep(200);
      }
   }

   pthread_exit(0);
   return 0;
}

//_____________________________________________________________________________
XrdConnectionMgr* XrdConnectionMgr::Instance() {
   // Create unique instance of the connection manager

   if(fgInstance == 0) {
      fgInstance = new XrdConnectionMgr;

      if(!fgInstance)
         abort();

   }
   return fgInstance;
}

//_____________________________________________________________________________
void XrdConnectionMgr::Reset()
{
   // Reset the connection manager

   SafeDelete(fgInstance);
   fgInstance = 0;
}

//____________________________________________________
XrdConnectionMgr::XrdConnectionMgr()
{
   // XrdConnectionMgr constructor.
   // Creates a Connection Manager object.
   // Starts the garbage collector thread.

   // Initialization of lock mutex
   pthread_mutexattr_t attr;
   int rc;

   // Initialization of lock mutex
   rc = pthread_mutexattr_init(&attr);
   if (rc == 0) {
      rc = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
      if (rc == 0)
	 rc = pthread_mutex_init(&fMutex, &attr);
   }
   if (rc) {
      Error("PhyConnection", 
            "Can't create mutex: out of system resources.");
      abort();
   }

   // Garbage collector thread creation
   if (DFLT_STARTGARBAGECOLLECTORTHREAD) {
      int pt_ret;

      pt_ret = pthread_create(&fGarbageColl, NULL, GarbageCollectorThread, this);
      if (pt_ret)
	 Error("ConnectionMgr",
	       "Can't create garbage collector thread: out of system resources");

   }
   else
      if(DebugLevel() >= XrdDebug::kHIDEBUG)
         Info(XrdDebug::kHIDEBUG,
	      "ConnectionMgr",
              "Explicitly requested not to start the garbage collector"
              " thread. Are you sure?");
}

//_____________________________________________________________________________
XrdConnectionMgr::~XrdConnectionMgr()
{
   // Deletes mutex locks, stops garbage collector thread.

   unsigned int i=0;

   {
      XrdMutexLocker mtx(fMutex);

      for (i = 0; i < fLogVec.size(); i++)
	 if (fLogVec[i]) Disconnect(i, FALSE);

   }

   pthread_cancel(fGarbageColl);
   pthread_join(fGarbageColl, 0);

   GarbageCollect();

   pthread_mutex_destroy(&fMutex);

   delete(fgInstance);
}

//_____________________________________________________________________________
void XrdConnectionMgr::GarbageCollect()
{
   // Get rid of unused physical connections. 'Unused' means not used for a
   // TTL time from any logical one. The TTL depends on the kind of remote
   // server. For a load balancer the TTL is very high, while for a data server
   // is quite small.

   // Mutual exclusion on the vectors and other vars
   {
      XrdMutexLocker mtx(fMutex);

      // We cycle all the physical connections
      for (unsigned short int i = 0; i < fPhyVec.size(); i++) { 
   
	 // If a single physical connection has no linked logical connections,
	 // then we kill it if its TTL has expired
	 if ( fPhyVec[i] && (GetPhyConnectionRefCount(fPhyVec[i]) <= 0) && 
	      fPhyVec[i]->ExpiredTTL() ) {
      
	    Info(XrdDebug::kDUMPDEBUG,
		 "GarbageCollect", "Purging physical connection " << i);

	    // Wait until the physical connection is unlocked (it may be in use by 
	    // slow processes)

	    fPhyVec[i]->Disconnect();
	    SafeDelete(fPhyVec[i]);
	    fPhyVec[i] = 0;
      
	    Info(XrdDebug::kHIDEBUG,
		 "GarbageCollect", "Purged physical connection " << i);

	 }
      }


   }

}

//_____________________________________________________________________________
short int XrdConnectionMgr::Connect(XrdUrlInfo RemoteServ)
{
   // Connects to the remote server:
   //  - Looks for an existing physical connection already bound to 
   //    RemoteAddress:TcpPort;
   //  - If needed, creates a TCP channel to RemoteAddress:TcpPort
   //    (this is a physical connection);
   //  - Creates a logical connection and binds it to the previous 
   //    created physical connection;
   //  - Returns the logical connection ID. Every client will use this
   //    ID to deal with the server.

   XrdLogConnection *logconn;
   XrdPhyConnection *phyconn;
   short int  newid;
   bool phyfound;

   // First we get a new logical connection object
   Info(XrdDebug::kHIDEBUG,
	"Connect", "Creating a logical connection...");

   logconn = new XrdLogConnection();
   if (!logconn) {
      Error("Connect", "Object creation failed. Aborting.");
      abort();
   }

   {
      XrdMutexLocker mtx(fMutex);

      // If we already have a physical connection to that host:port, 
      // then we use that
      phyfound = FALSE;

      for (unsigned short int i=0; i < fPhyVec.size(); i++) {
	 if ( fPhyVec[i] && fPhyVec[i]->IsValid() &&
	      fPhyVec[i]->IsPort(RemoteServ.Port) &&
	      (fPhyVec[i]->IsAddress(RemoteServ.Host) ||
	       fPhyVec[i]->IsAddress(RemoteServ.HostAddr)) ) {

	    // We link that physical connection to the new logical connection
	    fPhyVec[i]->Touch();
	    logconn->SetPhyConnection( fPhyVec[i] );
	    phyfound = TRUE;
	    break;
	 }
      }

   }

   if (!phyfound) {
      
      Info(XrdDebug::kHIDEBUG,
	   "Connect",
	   "Physical connection not found. Creating a new one...");

      // If not already present, then we must build a new physical connection, 
      // and try to connect it
      // While we are trying to connect, the mutex must be unlocked
      // Note that at this point logconn is a pure local instance, so it 
      // does not need to be protected by mutex
      phyconn = new XrdPhyConnection(this);

      if (!phyconn) {
	 Error("Connect", "Object creation failed. Aborting.");
         abort();
      }
      if ( phyconn && phyconn->Connect(RemoteServ) ) {

         logconn->SetPhyConnection(phyconn);

         if (DebugLevel() >= XrdDebug::kHIDEBUG)
            Info(XrdDebug::kHIDEBUG,
		 "Connect",
		 "New physical connection to server " <<
		 RemoteServ.Host << ":" << RemoteServ.Port <<
		 " succesfully created.");

      } else 
         return -1;
   }


   // Now, we are connected to the host desired.
   // The physical connection can be old or newly created
   {
      XrdMutexLocker mtx(fMutex);

      // Then, if needed, we push the physical connection into its vector
      if (!phyfound)
	 fPhyVec.push_back(phyconn);

      // Then we push the logical connection into its vector
      fLogVec.push_back(logconn);
 
      // Its ID is its position inside the vector, we must return it later
      newid = fLogVec.size()-1;

      // Now some debug log
      if (DebugLevel() >= XrdDebug::kHIDEBUG) {
	 int logCnt = 0, phyCnt = 0;

	 for (unsigned short int i=0; i < fPhyVec.size(); i++)
	    if (fPhyVec[i])
	       phyCnt++;
	 for (unsigned short int i=0; i < fLogVec.size(); i++)
	    if (fLogVec[i])
	       logCnt++;

	 Info(XrdDebug::kHIDEBUG,
	      "Connect",
	      "LogConn: size:" << fLogVec.size() << " count: " << logCnt <<
	      "PhyConn: size:" << fPhyVec.size() << " count: " << phyCnt );

      }

   }
  

   return newid;
}

//_____________________________________________________________________________
void XrdConnectionMgr::Disconnect(short int LogConnectionID, 
                                 bool ForcePhysicalDisc)
{
   // Deletes a logical connection.
   // Also deletes the related physical one if ForcePhysicalDisc=TRUE.
   if (LogConnectionID < 0) return;

   {
      XrdMutexLocker mtx(fMutex);

      if ((LogConnectionID < 0) ||
	  (LogConnectionID >= (int)fLogVec.size()) || (!fLogVec[LogConnectionID])) {
	 Error("Disconnect", "Destroying nonexistent logconn " << LogConnectionID);
	 return;
      }


      if (ForcePhysicalDisc) {
	 // We disconnect the phyconn
	 // But it will be removed by the garbagecollector as soon as possible
	 // Note that here we cannot destroy the phyconn, since there can be other 
	 // logconns pointing to it the phyconn will be removed when there are no 
	 // more logconns pointing to it
	 fLogVec[LogConnectionID]->GetPhyConnection()->SetTTL(0);
	 fLogVec[LogConnectionID]->GetPhyConnection()->Disconnect();
      }
    
      fLogVec[LogConnectionID]->GetPhyConnection()->Touch();
      SafeDelete(fLogVec[LogConnectionID]);
      fLogVec[LogConnectionID] = 0;


   }

}

//_____________________________________________________________________________
int XrdConnectionMgr::ReadRaw(short int LogConnectionID, void *buffer, 
                               int BufferLength)
{
   // Read BufferLength bytes from the logical connection LogConnectionID

   XrdLogConnection *logconn;

   logconn = GetConnection(LogConnectionID);

   if (logconn) {
      return logconn->ReadRaw(buffer, BufferLength);
   }
   else {
      Error("ReadRaw", "There's not a logical connection with id " <<
           LogConnectionID);

      return(TXSOCK_ERR);
   }
}

//_____________________________________________________________________________
XrdMessage *XrdConnectionMgr::ReadMsg(short int LogConnectionID)
{
   XrdLogConnection *logconn;
   XrdMessage *mex;

   logconn = GetConnection(LogConnectionID);


   // Parametric asynchronous stuff.
   // If we are going Sync, then we must build the message here,
   // otherwise the messages come directly from the queue
   if ( !DFLT_GOASYNC ) {

      // We get a new message directly from the socket.
      // The message gets inserted inside the phyconn queue
      // This line of code will be moved to a reader thread inside TXPhyConnection
      // Timeouts must not be ignored here, indeed they are an error
      // because we are waiting for a message that must come quickly
      mex = logconn->GetPhyConnection()->BuildMessage(FALSE, FALSE);

   }
   else {
      // Now we get the message from the queue, with the timeouts needed
      mex = logconn->GetPhyConnection()->ReadMessage(LogConnectionID);
   }

   // Return the message unmarshalled to ClientServerCmd
   return mex;
}

//_____________________________________________________________________________
int XrdConnectionMgr::WriteRaw(short int LogConnectionID, const void *buffer, 
                               int BufferLength) {
   // Write BufferLength bytes into the logical connection LogConnectionID

   XrdLogConnection *logconn;

   logconn = GetConnection(LogConnectionID);

   if (logconn) {
      return logconn->WriteRaw(buffer, BufferLength);
   }
   else {
      Error("WriteRaw", "There's not a logical connection with id " <<
	    LogConnectionID);

      return(TXSOCK_ERR);
   }
}

//_____________________________________________________________________________
XrdLogConnection *XrdConnectionMgr::GetConnection(short int LogConnectionID)
{
   // Return a logical connection object that has LogConnectionID as its ID.

   XrdLogConnection *res;

   {
      XrdMutexLocker mtx(fMutex);
 
      res = fLogVec[LogConnectionID];
   }
  
   return res;
}

//_____________________________________________________________________________
short int XrdConnectionMgr::GetPhyConnectionRefCount(XrdPhyConnection *PhyConn)
{
   // Return the number of logical connections bound to the physical one 'PhyConn'
   int cnt = 0;

   {
      XrdMutexLocker mtx(fMutex);

      for (unsigned short int i = 0; i < fLogVec.size(); i++)
	 if ( fLogVec[i] && (fLogVec[i]->GetPhyConnection() == PhyConn) ) cnt++;

   }
  
   return cnt;
}

//_____________________________________________________________________________
bool XrdConnectionMgr::ProcessUnsolicitedMsg(XrdUnsolicitedMsgSender *sender,
                                             XrdMessage *unsolmsg)
{
   // We are here if an unsolicited response comes from a physical connection
   // The response comes in the form of an TXMessage *, that must NOT be
   // destroyed after processing. It is destroyed by the first sender.
   // Remember that we are in a separate thread, since unsolicited responses
   // are asynchronous by nature.

   Info(XrdDebug::kNODEBUG,
	"ConnectionMgr", "Processing unsolicited response");

   // Local processing ....

   // Now we propagate the message to the interested objects.
   // In our architecture, the interested objects are the objects which
   // self-registered in the logical connections belonging to the Phyconn
   // which threw the event
   // So we throw the evt towards each logical connection
   {
      XrdMutexLocker mtx(fMutex);

      for (unsigned short int i = 0; i < fLogVec.size(); i++)
	 if ( fLogVec[i] && (fLogVec[i]->GetPhyConnection() == sender) ) {
	    fLogVec[i]->ProcessUnsolicitedMsg(sender, unsolmsg);
	 }

   }


   return TRUE;
}
