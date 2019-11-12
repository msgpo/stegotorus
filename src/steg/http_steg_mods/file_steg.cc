/** 
  The implmentation for non-abstract methods of FileStegMod Class

  AUTHORS: Vmon July 2013, Initial version
*/

#include <vector>
#include <algorithm>
#include <event2/buffer.h>
#include <assert.h>

#include <fstream> //for decode failure test

#include <math.h>

using namespace std;

#include "util.h"
#include "evbuf_util.h" //why do I need this now?
#include "../payload_server.h"
//#include "jsSteg.h"

#include "file_steg.h"
#include "connections.h"

// error codes
#define INVALID_BUF_SIZE  -1
#define INVALID_DATA_CHAR -2

// controlling content gzipping for jsSteg
#define JS_GZIP_RESP             1

/**
  constructor, sets the playoad server

  @param the payload server that is going to be used to provide cover
         to this module.
*/
FileStegMod::FileStegMod(PayloadServer& payload_provider, double noise2signal_from_cfg, int child_type = -1)
  :_payload_server(payload_provider), noise2signal(noise2signal_from_cfg), c_content_type(child_type), outbuf(c_HTTP_PAYLOAD_BUF_SIZE)
{
  log_debug("max storage size: %lu >= maxs preceived storage: %lu >= max no of bits needed for storge %f", sizeof(message_size_t),
            c_NO_BYTES_TO_STORE_MSG_SIZE, log2(c_MAX_MSG_BUF_SIZE)/8.0);
            
  log_assert(sizeof(message_size_t) >= c_NO_BYTES_TO_STORE_MSG_SIZE);
  log_assert(c_NO_BYTES_TO_STORE_MSG_SIZE >= log2(c_MAX_MSG_BUF_SIZE)/8.0);

}


/**
   Encapsulate the repetative task of checking for the respones of content_type
   choosing one with appropriate size and extracting the body from header

   @param data_len: the length of data being embed should be < capacity
   @param payload_buf: the http response (header+body) corresponding going to cover the data
   @param payload_size: the size of the payload_buf

   @return the offset of the body content of payload_buf or < 0 in case of
           error, that is RESPONSE_INCOMPLETE (<0) if it is incomplete (can't
           find the start of body) or RESPONSE_BAD (<0) in case of other
           errors
*/
ssize_t 
FileStegMod::extract_appropriate_respones_body(const std::vector<uint8_t>& payload_buf)
{
  //TODO: this need to be investigated, we might need two functions
  const static std::vector<uint8_t> end_of_header = {'\r', '\n', '\r', '\n'};
  auto hend = std::search(payload_buf.begin(), payload_buf.end(), end_of_header.begin(), end_of_header.end()); 
  if (hend == payload_buf.end()) {
    //log_debug("%s", payload_buf);
    log_debug("unable to find end of header in the HTTP template");
    return -1;
  }

  //hLen = hend+4-*payload_buf;
  return hend-payload_buf.begin()+end_of_header.size();

}

/**
   The overloaded version with evbuffer

   @param payload_buf can not be defined constant as evbuffer_search 
                      doesn't accept const.
*/
ssize_t 
FileStegMod::extract_appropriate_respones_body(evbuffer* payload_buf)
{
  //TODO: this need to be investigated, we might need two functions
  const evbuffer_ptr hend = evbuffer_search(payload_buf, "\r\n\r\n", sizeof ("\r\n\r\n") -1 , NULL);
  if (hend.pos == -1) {
    log_debug("unable to find end of header in the HTTP respose");
    return RESPONSE_INCOMPLETE;
  }

  return hend.pos + strlen("\r\n\r\n");

}

/**
   Finds a payload of approperiate type and size and copy it into payload_buf

   @param data_len: the payload should be able to accomodate this length
   @param payload_buf: the buff that is going to contain the chosen payload

   @return payload size or < 0 in case of error
*/
ssize_t FileStegMod::pick_appropriate_cover_payload(size_t data_len, const std::vector<uint8_t>* payload_buf, string& cover_id_hash)
{
  size_t max_capacity = _payload_server._payload_database.typed_maximum_capacity(c_content_type);

  if (max_capacity <= 0) {
    log_abort("SERVER ERROR: No payload of appropriate type=%d was found\n", (int) c_content_type);
    return -1;
  }

  if (data_len > (size_t) max_capacity) {
    log_abort("SERVER ERROR: type %d cannot accommodate data %d",
             (int) c_content_type, (int) data_len);
    return -1;
  }

  do {
    if (_payload_server.get_payload(c_content_type, data_len, payload_buf, noise2signal, &cover_id_hash) == 1) {
      log_debug("SERVER found the next HTTP response template with size %d",
                (int)payload_buf->size());
    } else { //we can't do much here anymore, we need to add payload to payload
      //database unless if the payload_server is serving randomly which means
      //next time probably won't serve a corrupted payload
      log_warn("SERVER couldn't find the next HTTP response template, enrich payload database and restart Stegotorus");
      return -1;
    }
  } while(payload_buf->size() == 0); //if the payload size is zero it means that we have failed
  //in retrieving the file and it might be helpful to try to download it again.

  return payload_buf->size();

}

/**
   Find appropriate payload calls virtual embed to embed it appropriate
   to its type
   //TODO: also consolidate source buffer if it is scattered. That is why source
   //      can not be a const. this is violation
   // no side effect principal and this should be done explicitly somewhere else.
   // If the function needs  straighten buffer to transmit, then it should not 
   // accept a buffer but a memory block to begin with?

   @param source the data to be transmitted, 
   @param conn the connection over which the data is going to be transmitted

   @return the number of bytes transmitted
*/
int
FileStegMod::http_server_transmit(evbuffer *source, conn_t *conn)
{
  vector<uint8_t> data_to_be_transferred;
  int sbuflen = 0;

  ssize_t outbuflen = 0;
  ssize_t body_offset = 0;
  vector<uint8_t> newHdr;
  ssize_t newHdrLen = 0;
  ssize_t cnt = 0;
  size_t body_len = 0;
  size_t hLen = 0;

  evbuffer *dest;

  //call this from util to extract the buffer into memory block
  //data1 is allocated in evbuffer_to_memory_block we need to free
  //it at the end.
  sbuflen = evbuffer_to_memory_block(source, data_to_be_transferred);

  if (sbuflen < 0 /*&& c_content_type != HTTP_CONTENT_JAVASCRIPT || CONTENT_HTML_JAVASCRIPT*/) {
    log_warn("unable to extract the data from evbuffer");
    return -1;
  }

  //now we need to choose a payload. If a cover failed we through it out and try again
  const std::vector<uint8_t>* cover_payload = nullptr;
  string payload_id_hash;
  do  {
    cnt = pick_appropriate_cover_payload(sbuflen, cover_payload, payload_id_hash);
    if (cnt < 0) {
      log_warn("Failed to aquire approperiate payload."); //if there is no approperiate cover of this type
      //then we can't continue :(
      return -1;
    }

    //we reached here so we must have a valid cover
    log_assert(cover_payload != nullptr);

    //we shouldn't touch the cover as there is only one copy of it in the
    //the cache
    //log_debug("cover body: %s",cover_payload);
    body_offset =  extract_appropriate_respones_body(*cover_payload);
    if (body_offset < 0) {
      log_warn("Failed to aquire approperiate payload.");
      _payload_server.disqualify_payload(payload_id_hash);
      continue; //we try with another cover
    }

    body_len = cnt-body_offset;
    hLen = body_offset;
    log_debug("coping body of %zu size", (body_len));
    if ((body_len) > c_HTTP_PAYLOAD_BUF_SIZE) {
      log_warn("HTTP response doesn't fit in the buffer %zu > %zu", (body_len)*sizeof(char), c_HTTP_PAYLOAD_BUF_SIZE);
      _payload_server.disqualify_payload(payload_id_hash);
      return -1;
    }
    outbuf.insert(outbuf.begin(), cover_payload->begin() + body_offset, cover_payload->end());

    //int hLen = body_offset - (size_t)cover_payload - 4 + 1;
    //extrancting the body part of the payload
    log_debug("SERVER embeding transfer buffer with length %d into type %d", sbuflen, c_content_type);
    outbuflen = encode(data_to_be_transferred, outbuf);

    ///End of steg test!!
    if (outbuflen < 0) {
      log_warn("SERVER embedding fails");
      _payload_server.disqualify_payload(payload_id_hash);
      
    }
  } while(outbuflen < 0); //If we fail to embed, it is probably because
    //the cover had problem, we try again with different cover
    
  //At this point body_len isn't valid anymore
  //we should only use outbuflen, cause the stegmodule might
  //have changed the original body_len

  //If everything seemed to be fine, New steg module test:
  if (!(LOG_SEV_DEBUG < log_get_min_severity())) { //only perform this during debug
    std::vector<uint8_t> recovered_data_for_test; //this is the size we have promised to decode func
    decode(outbuf, recovered_data_for_test);

    if ((data_to_be_transferred.size() != recovered_data_for_test.size()) ||
        (!std::equal(data_to_be_transferred.begin(), data_to_be_transferred.end(), recovered_data_for_test.begin()))) { //barf!!
      //keep the evidence for testing
     // if(pgenflag == FILE_PAYLOAD)
     //{
      	ofstream failure_evidence_file("fail_cover.log", ios::binary | ios::out);
      	failure_evidence_file.write(reinterpret_cast<const char*>(cover_payload->data() + body_offset), body_len);
      	failure_evidence_file.write(reinterpret_cast<const char*>(cover_payload->data() + body_offset), body_len);
      	failure_evidence_file.close();
     //}
      ofstream failure_embed_evidence_file("failed_embeded_cover.log", ios::binary | ios::out);
      failure_embed_evidence_file.write(reinterpret_cast<const char*>(outbuf.data()), outbuflen);
      failure_embed_evidence_file.close();
      log_warn("decoding cannot recovers the encoded data consistantly for type %d", c_content_type);
      goto error;
    }
  }

  log_debug("SERVER FileSteg sends resp with hdr len %zu body len %zd",
            body_offset, outbuflen);
 
  //Update: we can't assert this anymore, SWFSteg changes the size
  //so this equality doesn't hold anymore
  //assert((size_t)outbuflen == body_len); //changing length is not supported yet
  //instead we need to check if SWF or PDF, the payload length is changed
  //and in that case we need to update the header
  
	/*if( c_content_type == HTTP_CONTENT_JAVASCRIPT) {
	  //TODO instead of generating the header we should just manipulate
  //it
  //The only possible problem is length but we are not changing 
  //the length for now
   newHdrLen = gen_response_header((char*) "application/x-javascript", gzipMode, outbuflen, (char *)newHdr, sizeof((char*)newHdr));
  if (newHdrLen < 0) {
    log_warn("SERVER ERROR: gen_response_header fails for JSSteg");
    return -1;
    }
   }
  //I'm not crazy, these are filler for later change*/

  if ((size_t)outbuflen == body_len) {
    log_assert(hLen < MAX_RESP_HDR_SIZE);
    newHdr.assign(cover_payload->begin(), cover_payload->begin()+hLen);
    newHdrLen = hLen;
     
  }
  else { //if the length is different, then we need to update the header
   alter_length_in_response_header(*cover_payload, outbuflen, newHdr);
   newHdrLen = newHdr.size();
    if (!newHdrLen) {
      log_warn("SERVER ERROR: failed to alter length field in response headerr");
      _payload_server.disqualify_payload(payload_id_hash);
      goto error;
    }
  }

  dest = conn->outbound();
  if (evbuffer_add(dest, newHdr.data(), newHdrLen)) {
    log_warn("SERVER ERROR: evbuffer_add() fails for newHdr");
    goto error;
    }

  if (evbuffer_add(dest, outbuf.data(), outbuflen)) {
    log_warn("SERVER ERROR: evbuffer_add() fails for outbuf");
    goto error;
    return -1;
  }

  evbuffer_drain(source, sbuflen);
  return outbuflen;

 error:
  return -1;

}

int
FileStegMod::http_client_receive(conn_t *conn, struct evbuffer *dest,
                               struct evbuffer* source)
{
  unsigned int response_len = 0;
  int content_len = 0, outbuflen;
  uint8_t *httpHdr, *httpBody;

  log_debug("Entering CLIENT receive");

  ssize_t body_offset = extract_appropriate_respones_body(source);
  if (body_offset == RESPONSE_INCOMPLETE) {
    log_debug("CLIENT Did not find end of HTTP header %d, Incomplete Response",
             (int) evbuffer_get_length(source));
    return RECV_INCOMPLETE;
  }

  log_debug("CLIENT received response header with len %d", (int)body_offset-4);

  response_len = 0;
  ssize_t hdrLen = body_offset;
  response_len += hdrLen;

  httpHdr = evbuffer_pullup(source, hdrLen);
  if (httpHdr == NULL) {
    log_warn("CLIENT unable to pullup the complete HTTP header");
    return RECV_BAD;
  }

  content_len = find_content_length((char*)httpHdr, hdrLen);
  if (content_len < 0) {
    log_warn("CLIENT unable to find content length");
    return RECV_BAD;
  }
  log_debug("CLIENT received Content-Length = %d\n", content_len);

  response_len += content_len;

  if (response_len > evbuffer_get_length(source)) {
    log_debug("Incomplete response, waiting for more data.");
    return RECV_INCOMPLETE;
  }

  httpHdr = evbuffer_pullup(source, response_len);

  if (httpHdr == NULL) {
    log_warn("CLIENT unable to pullup the complete HTTP body");
    return RECV_BAD;
  }

  httpBody = httpHdr + hdrLen;
  log_debug("CLIENT unwrapping data out of type %d payload", c_content_type);

  outbuflen = decode(httpBody, content_len, outbuf);
  if (outbuflen < 0) {
    log_warn("CLIENT ERROR: FileSteg fails\n");
    return RECV_BAD;
  }

  log_debug("CLIENT unwrapped data of length %d:", outbuflen);

  if (evbuffer_add(dest, outbuf.data(), outbuflen)) {
    log_warn("CLIENT ERROR: evbuffer_add to dest fails\n");
    return RECV_BAD;
  }

  if (evbuffer_drain(source, response_len) == -1) {
    log_warn("CLIENT ERROR: failed to drain source\n");
    return RECV_BAD;
  }

  conn->expect_close();
  return RECV_GOOD;

}

void
FileStegMod::alter_length_in_response_header(const std::vector<uint8_t>& payload_with_original_header, ssize_t new_content_length, std::vector<uint8_t>& new_header)
{
  static const string length_field_name = "Content-Length:";
  static const string end_of_field_indicator = "\r\n";

  //TODO: replace these with vector search
  auto length_field_start = std::search(payload_with_original_header.begin(), payload_with_original_header.end(), length_field_name.begin(), length_field_name.end());
  if (length_field_start == payload_with_original_header.end()) {
    log_warn("payload with bad header. unable to find the Content-Length field");
    return;
  }
  
  length_field_start += length_field_name.length();

  auto length_field_end = std::search(length_field_start, payload_with_original_header.end(), end_of_field_indicator.begin(), end_of_field_indicator.end());
  if (length_field_end ==  payload_with_original_header.end()) {
    log_warn("payload with bad header. unable to find the end of Content-Length field.");
    return;
  }

  
  //copy the first part of the header
  new_header.insert(new_header.end(), payload_with_original_header.begin(), length_field_start);
  //memcpy(new_header, payload_with_original_header.data(), ((uint8_t*)length_field_start - original_header));
  string new_content_length_str(std::to_string(new_content_length));

  new_header.insert(new_header.end(), new_content_length_str.begin(), new_content_length_str.end());
  //copy the rest of the header
  new_header.insert(new_header.end(), length_field_end,  payload_with_original_header.end());

}
 	

