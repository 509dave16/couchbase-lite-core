//
//  DBActor.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/21/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "DBActor.hh"
#include "Pusher.hh"
#include "FleeceCpp.hh"
#include "BLIPConnection.hh"
#include "Message.hh"
#include "SecureDigest.hh"
#include "c4.hh"

using namespace std;
using namespace fleece;
using namespace litecore::blip;

namespace litecore { namespace repl {

    static bool isNotFoundError(C4Error err) {
        return err.domain == LiteCoreDomain && err.code == kC4ErrorNotFound;
    }


    void DBActor::setConnection(blip::Connection *connection) {
        ReplActor::setConnection(connection);
        registerHandler("getCheckpoint",    &DBActor::handleGetCheckpoint);
        registerHandler("changes",          &DBActor::handleChanges);
    }


    void DBActor::_getCheckpoint(CheckpointCallback callback) {
        alloc_slice checkpointID(effectiveRemoteCheckpointDocID());
        C4Error err;
        c4::ref<C4RawDocument> doc( c4raw_get(_db,
                                              C4STR("checkpoints"),
                                              checkpointID,
                                              &err) );
        if (doc) {
            callback(checkpointID, doc->body, doc->meta, {});
        } else {
            if (isNotFoundError(err))
                err = {};
            callback(checkpointID, alloc_slice(), alloc_slice(), err);
        }
    }


    slice DBActor::effectiveRemoteCheckpointDocID() {
        if (_remoteCheckpointDocID.empty()) {
            // Simplistic default value derived from db UUID and remote URL:
            C4UUID privateUUID;
            C4Error err;
            if (!c4db_getUUIDs(_db, nullptr, &privateUUID, &err))
                throw "fail";//FIX
            fleeceapi::Encoder enc;
            enc.beginArray();
            enc.writeString({&privateUUID, sizeof(privateUUID)});
            enc.writeString(_remoteAddress);
            enc.endArray();
            alloc_slice data = enc.finish();
            SHA1 digest(data);
            _remoteCheckpointDocID = string("cp-") + slice(&digest, sizeof(digest)).base64String();
        }
        return slice(_remoteCheckpointDocID);
    }


    void DBActor::handleGetCheckpoint(Retained<MessageIn> request) {
        auto checkpointID = request->property("client"_sl);
        if (!checkpointID)
            return request->respondWithError("BLIP"_sl, 400);
        LogTo(SyncLog, "Request for checkpoint '%.*s'", SPLAT(checkpointID));

        C4Error err;
        c4::ref<C4RawDocument> doc( c4raw_get(_db, C4STR("peerCheckpoints"), checkpointID, &err) );
        if (doc) {
            MessageBuilder response(request);
            response["rev"_sl] = doc->meta;
            response << doc->body;
            request->respond(response);
        } else if (isNotFoundError(err)) {
            return request->respondWithError("HTTP"_sl,
                                             isNotFoundError(err) ? 404 : 502);
        }
    }

    
    // A request from the Pusher to send it a batch of changes. Will respond by calling gotChanges.
    void DBActor::_getChanges(C4SequenceNumber since, unsigned limit, bool continuous,
                              Retained<Pusher> pusher) {
        LogTo(SyncLog, "Reading %u local changes from %llu", limit, since);
        vector<Rev> changes;
        C4Error error = {};
        C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
        options.flags &= ~kC4IncludeBodies;
        options.flags |= kC4IncludeDeleted;
        auto e = c4db_enumerateChanges(_db, since, &options, &error);
        if (e) {
            changes.reserve(limit);
            while (c4enum_next(e, &error) && limit-- > 0) {
                C4DocumentInfo info;
                c4enum_getDocumentInfo(e, &info);
                changes.emplace_back(info);
            }
            c4enum_free(e);
        }

        if (continuous && changes.empty() && !_changeObserver) {
            // Reached the end of history; now start observing for future changes
            _changeObserver = c4dbobs_create(_db, &changeCallback, this);
        }

        pusher->gotChanges(changes, error);
        //TODO: Handle continuous mode (start observer)
    }


    void DBActor::changeCallback(C4DatabaseObserver* observer, void *context) {
        ((DBActor*)context)->dbChanged();
    }

    
    void DBActor::dbChanged() {

    }


    // Sends a document revision in a "rev" request.
    void DBActor::_sendRevision(Rev rev, vector<string> ancestors, int maxHistory) {
        LogVerbose(SyncLog, "Sending revision '%.*s' #%.*s", SPLAT(rev.docID), SPLAT(rev.revID));
        C4Error c4err;
        auto doc = c4doc_get(_db, rev.docID, true, &c4err);
        if (!doc)
            return gotError(c4err);
        if (!c4doc_selectRevision(doc, rev.revID, true, &c4err))
            return gotError(c4err);

        alloc_slice body = doc->selectedRev.body;

        // Generate the revision history string:
        stringstream historyStream;
        for (int n = 0; n < maxHistory; ++n) {
            if (!c4doc_selectParentRevision(doc))
                break;
            string revID = slice(doc->selectedRev.revID).asString();
            if (n > 0)
                historyStream << ',';
            historyStream << revID;
            if (find(ancestors.begin(), ancestors.end(), revID) != ancestors.end())
                break;
        }
        string history = historyStream.str();

        // Now send the BLIP message:
        MessageBuilder msg("rev"_sl);
        msg.noreply = true;                 //FIX: Sometimes need a reply
        msg["id"_sl] = rev.docID;
        msg["rev"_sl] = rev.revID;
        msg["sequence"_sl] = rev.sequence;
        if (!history.empty())
            msg["history"_sl] = history;
        msg.write(body);
        sendRequest(msg);
    }


    void DBActor::handleChanges(Retained<MessageIn> req) {
        LogTo(SyncLog, "Handling 'changes' message");
        auto changes = req->JSONBody().asArray();
        if (!changes) {
            LogToAt(SyncLog, Warning, "Invalid body of 'changes' message");
            req->respondWithError("BLIP"_sl, 400);
            return;
        }

        if (req->noReply())
            return;

        LogTo(SyncLog, "Looking up %u revisions in the db ...", changes.count());
        MessageBuilder response(req);
        response["maxRevs"_sl] = c4db_getMaxRevTreeDepth(_db);
        unsigned i = 0, itemsWritten = 0, requested = 0;
        vector<alloc_slice> ancestors;
        auto &encoder = response.jsonBody();
        encoder.beginArray();
        for (auto item : changes) {
            auto change = item.asArray();
            slice docID = change[0].asString();
            slice revID = change[2].asString();
            if (!docID || !revID) {
                LogToAt(SyncLog, Warning, "Invalid entry in 'changes' message");
                return;     // ???  Should this abort the replication?
            }

            if (!findAncestors(docID, revID, ancestors)) {
                ++requested;
                while (++itemsWritten < i)
                    encoder.writeInt(0);
                encoder.beginArray();
                for (slice ancestor : ancestors)
                    encoder.writeString(ancestor);
                encoder.endArray();
            }
            ++i;
        }
        encoder.endArray();
        LogTo(SyncLog, "Responding w/request for %u revs", requested);
        req->respond(response);
    }


    // Returns true if revision exists; else returns false and sets ancestors to an array of
    // ancestor revisions I do have (empty if doc doesn't exist at all)
    bool DBActor::findAncestors(slice docID, slice revID, vector<alloc_slice> &ancestors) {
        C4Error err;
        c4::ref<C4Document> doc = c4doc_get(_db, docID, true, &err);
        bool revExists = doc && c4doc_selectRevision(doc, revID, false, &err);
        if (!revExists) {
            ancestors.resize(0);
            if (!isNotFoundError(err)) {
                gotError(err);
            } else if (doc) {
                // Revision isn't found, but look for ancestors:
                if (c4doc_selectFirstPossibleAncestorOf(doc, revID)) {
                    do {
                        ancestors.emplace_back(doc->selectedRev.revID);
                    } while (c4doc_selectNextPossibleAncestorOf(doc, revID)
                             && ancestors.size() < kMaxPossibleAncestors);
                }
            }
        }
        return revExists;
    }

} }
