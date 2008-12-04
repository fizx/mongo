// instance.cpp : Global state variables and functions.
//

/**
*    Copyright (C) 2008 10gen Inc.
*  
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*  
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*  
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "stdafx.h"
#include "db.h"
#include "query.h"
#include "introspect.h"
#include "repl.h"
#include "dbmessage.h"
#include "instance.h"

bool slave = false;
bool master = false; // true means keep an op log
extern int curOp;

boost::mutex dbMutex;
int dbLocked = 0;

int port = DBPort;
/* 0 = off; 1 = writes, 2 = reads, 3 = both 
   7 = log a few reads, and all writes.
*/
int opLogging = 0;
int getOpLogging() { return opLogging; }
OpLog _oplog;
//#define oplog (*(_oplog.f))

bool useCursors = true;

void closeAllSockets();
void flushOpLog() { _oplog.flush(); }

void killCursors(int n, long long *ids);
void receivedKillCursors(Message& m) {
	int *x = (int *) m.data->_data;
	x++; // reserved
	int n = *x++;
	assert( n >= 1 );
	if( n > 2000 ) { 
		problem() << "Assertion failure, receivedKillCursors, n=" << n << endl;
		assert( n < 30000 );
	}
	killCursors(n, (long long *) x);
}

void receivedUpdate(Message& m, stringstream& ss) {
	DbMessage d(m);
	const char *ns = d.getns();
	assert(*ns);
	setClient(ns);
	//if( database->profile )
    ss << ns << ' ';
	int flags = d.pullInt();
	BSONObj query = d.nextJsObj();

	assert( d.moreJSObjs() );
	assert( query.objsize() < m.data->dataLen() );
	BSONObj toupdate = d.nextJsObj();

	assert( toupdate.objsize() < m.data->dataLen() );
	assert( query.objsize() + toupdate.objsize() < m.data->dataLen() );
	updateObjects(ns, toupdate, query, flags & 1, ss);
}

void receivedDelete(Message& m) {
	DbMessage d(m);
	const char *ns = d.getns();
	assert(*ns);
	setClient(ns);
	int flags = d.pullInt();
	bool justOne = flags & 1;
	assert( d.moreJSObjs() );
	BSONObj pattern = d.nextJsObj();
	deleteObjects(ns, pattern, justOne);
	logOp("d", ns, pattern, 0, &justOne);
}

void receivedQuery(DbResponse& dbresponse, /*AbstractMessagingPort& dbMsgPort, */Message& m, stringstream& ss, bool logit) {
	MSGID responseTo = m.data->id;

	DbMessage d(m);
    QueryMessage q(d);

	if( opLogging && logit ) { 
		if( strstr(q.ns, ".$cmd") ) { 
			/* $cmd queries are "commands" and usually best treated as write operations */
			OPWRITE;
		}
		else { 
			OPREAD;
		}
	}

	setClient(q.ns);
	QueryResult* msgdata;

	try { 
		msgdata = runQuery(m, q.ns, q.ntoskip, q.ntoreturn, q.query, q.fields, ss, q.queryOptions);
	}
	catch( AssertionException& e ) { 
		ss << " exception ";
		problem() << " Caught Assertion in runQuery ns:" << q.ns << ' ' << e.toString() << '\n';
		log() << "  ntoskip:" << q.ntoskip << " ntoreturn:" << q.ntoreturn << '\n';
        if( q.query.valid() )
            log() << "  query:" << q.query.toString() << endl;
        else
            log() << "  query object is not valid!" << endl;

        BSONObjBuilder err;
        err.append("$err", e.msg.empty() ? "assertion during query" : e.msg);
        BSONObj errObj = err.done();

        BufBuilder b;
        b.skip(sizeof(QueryResult));
        b.append((void*) errObj.objdata(), errObj.objsize());

        // todo: call replyToQuery() from here instead of this.  needs a little tweaking
        // though to do that.
        msgdata = (QueryResult *) b.buf();
        b.decouple();
		QueryResult *qr = msgdata;
		qr->_data[0] = 0;
		qr->_data[1] = 0;
		qr->_data[2] = 0;
		qr->_data[3] = 0;
		qr->len = b.len();
		qr->setOperation(opReply);
		qr->cursorId = 0;
		qr->startingFrom = 0;
		qr->nReturned = 1;

	}
	Message *resp = new Message();
	resp->setData(msgdata, true); // transport will free
	dbresponse.response = resp;
	dbresponse.responseTo = responseTo;
	if( database ) { 
		if( database->profile )
			ss << " bytes:" << resp->data->dataLen();
	}
	else { 
		if( strstr(q.ns, "$cmd") == 0 ) // (this condition is normal for $cmd dropDatabase)
			log() << "ERROR: receiveQuery: database is null; ns=" << q.ns << endl;
	}
	//	dbMsgPort.reply(m, resp, responseTo);
}

QueryResult* emptyMoreResult(long long);

void receivedGetMore(DbResponse& dbresponse, /*AbstractMessagingPort& dbMsgPort, */Message& m, stringstream& ss) {
	DbMessage d(m);
	const char *ns = d.getns();
	ss << ns;
	setClient(ns);
	int ntoreturn = d.pullInt();
	long long cursorid = d.pullInt64();
	ss << " cid:" << cursorid;
	ss << " ntoreturn:" << ntoreturn;
	QueryResult* msgdata;
	try { 
		msgdata = getMore(ns, ntoreturn, cursorid);
	}
	catch( AssertionException& e ) { 
		ss << " exception " + e.toString();
		msgdata = emptyMoreResult(cursorid);
	}
	Message *resp = new Message();
	resp->setData(msgdata, true);
	ss << " bytes:" << resp->data->dataLen();
	ss << " nreturned:" << msgdata->nReturned;
	dbresponse.response = resp;
	dbresponse.responseTo = m.data->id;
	//dbMsgPort.reply(m, resp);
}

void receivedInsert(Message& m, stringstream& ss) {
//	cout << "GOT MSG id:" << m.data->id << endl;
	DbMessage d(m);
	while( d.moreJSObjs() ) {
		BSONObj js = d.nextJsObj();
		const char *ns = d.getns();
		assert(*ns);
		setClient(ns);
		ss << ns;
		theDataFileMgr.insert(ns, (void*) js.objdata(), js.objsize());
		logOp("i", ns, js);
	}
}

int ctr = 0;
extern int callDepth;

class JniMessagingPort : public AbstractMessagingPort { 
public:
	JniMessagingPort(Message& _container) : container(_container) { }
	void reply(Message& received, Message& response, MSGID) {
		container = response;
	}
	void reply(Message& received, Message& response) { 
		container = response;
	}
	Message & container;
};

/* a call from java/js to the database locally. 

     m - inbound message
     out - outbound message, if there is any, will be set here.
           if there is one, out.data will be non-null on return.
		   The out.data buffer will automatically clean up when out
		   goes out of scope (out.freeIt==true)

   note we should already be in the mutex lock from connThread() at this point.
*/
void jniCallback(Message& m, Message& out)
{
	Database *clientOld = database;

	JniMessagingPort jmp(out);
	callDepth++;
	int curOpOld = curOp;

	try { 

		stringstream ss;
		char buf[64];
		time_t_to_String(time(0), buf);
		buf[20] = 0; // don't want the year
		ss << buf << " dbjs ";

		{
			Timer t;

			bool log = false;
			curOp = m.data->operation();

			if( m.data->operation() == dbQuery ) { 
				// on a query, the Message must have m.freeIt true so that the buffer data can be 
				// retained by cursors.  As freeIt is false, we make a copy here.
				assert( m.data->len > 0 && m.data->len < 32000000 );
				Message copy(malloc(m.data->len), true);
				memcpy(copy.data, m.data, m.data->len);
				DbResponse dbr;
				receivedQuery(dbr, copy, ss, false);
				jmp.reply(m, *dbr.response, dbr.responseTo);
			}
			else if( m.data->operation() == dbInsert ) {
				ss << "insert ";
				receivedInsert(m, ss);
			}
			else if( m.data->operation() == dbUpdate ) {
				ss << "update ";
				receivedUpdate(m, ss);
			}
			else if( m.data->operation() == dbDelete ) {
				ss << "remove ";
				receivedDelete(m);
			}
			else if( m.data->operation() == dbGetMore ) {
				DEV log = true;
				ss << "getmore ";
				DbResponse dbr;
				receivedGetMore(dbr, m, ss);
				jmp.reply(m, *dbr.response, dbr.responseTo);
			}
			else if( m.data->operation() == dbKillCursors ) { 
				try {
					log = true;
					ss << "killcursors ";
					receivedKillCursors(m);
				}
				catch( AssertionException& ) { 
					problem() << "Caught Assertion in kill cursors, continuing" << endl; 
					ss << " exception ";
				}
			}
			else {
				cout << "    jnicall: operation isn't supported: " << m.data->operation() << endl;
				assert(false);
			}

			int ms = t.millis();
			log = log || ctr++ % 128 == 0;
			if( log || ms > 100 ) {
				ss << ' ' << t.millis() << "ms";
				cout << ss.str().c_str() << endl;
			}
			if( database && database->profile >= 1 ) { 
				if( database->profile >= 2 || ms >= 100 ) { 
					// profile it
					profile(ss.str().c_str()+20/*skip ts*/, ms);
				}
			}
		}

	}
	catch( AssertionException& ) { 
		problem() << "Caught AssertionException in jniCall()" << endl;
	}

	curOp = curOpOld;
	callDepth--;

	if( database != clientOld ) { 
		database = clientOld;
		wassert(false);
	}
}

#undef exit
void dbexit(int rc, const char *why) { 
	log() << "  dbexit: " << why << "; flushing op log and files" << endl;
	flushOpLog();

	/* must do this before unmapping mem or you may get a seg fault */
	closeAllSockets();

	MemoryMappedFile::closeAllFiles();
	log() << "  dbexit: really exiting now" << endl;
	exit(rc);
}