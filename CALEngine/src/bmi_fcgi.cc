#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <fcgio.h>
#include <algorithm>
#include "utils/simple-cmd-line-helper.h"
#include "bmi_para.h"
#include "bmi_para_scal.h"
#include "bmi_doc_scal.h"
#include "features.h"
#include "utils/feature_parser.h"
#include "utils/utils.h"

using namespace std;
unordered_map<string, unique_ptr<BMI>> SESSIONS;
unique_ptr<Dataset> documents = nullptr;
unique_ptr<Dataset> test_documents = nullptr;
unique_ptr<ParagraphDataset> paragraphs = nullptr;
unordered_map<uint32_t, string> id_tokens;
unordered_map<string, int> test_labels_map;


// Get the uri without following and preceding slashes
string parse_action_from_uri(string uri){
    int st = 0, end = int(uri.length())-1;

    while(st < uri.length() && uri[st] == '/') st++;
    while(end >= 0 && uri[end] == '/') end--;

    return uri.substr(st, end - st + 1);
}

// Use a proper library someday
// returns a vector of <key, value> in given url query string
vector<pair<string, string>> parse_query_string(string query_string){
    vector<pair<string, string>> key_vals;
    string key, val;
    bool valmode = false;
    for(char ch: query_string){
        if(ch == '&'){
            if(key.length() > 0)
                key_vals.push_back({key, val});
            key = val = "";
            valmode = false;
        }else if(ch == '=' && !valmode){
            valmode = true;
        }else{
            if(valmode)
                val.push_back(ch);
            else
                key.push_back(ch);
        }
    }
    if(key.length() > 0)
        key_vals.push_back({key, val});
    return key_vals;
}

// returns the content field of request
// Built upon http://chriswu.me/blog/writing-hello-world-in-fcgi-with-c-plus-plus/
string read_content(const FCGX_Request & request){
    fcgi_streambuf cin_fcgi_streambuf(request.in);
    istream request_stream(&cin_fcgi_streambuf);

    char * content_length_str = FCGX_GetParam("CONTENT_LENGTH", request.envp);
    unsigned long content_length = 0;

    if (content_length_str) {
        content_length = atoi(content_length_str);
    }

    char * content_buffer = new char[content_length];
    request_stream.read(content_buffer, content_length);
    content_length = request_stream.gcount();

    do request_stream.ignore(1024); while (request_stream.gcount() == 1024);
    string content(content_buffer, content_length);
    delete [] content_buffer;
    return content;
}

// Given info write to the request's response
void write_response(const FCGX_Request & request, int status, string content_type, string content){
    fcgi_streambuf cout_fcgi_streambuf(request.out);
    ostream response_stream(&cout_fcgi_streambuf);
    response_stream << "Status: " << to_string(status) << "\r\n"
                    << "Content-type: " << content_type << "\r\n"
                    << "\r\n"
                    << content << "\n";
    /* if(content.length() > 50) */
    /*     content = content.substr(0, 50) + "..."; */
    cerr<<"Wrote response: "<<content<<endl;
}

bool parse_seed_judgments(const string &str, vector<pair<string, int>> &seed_judgments){
    size_t cur_idx = 0;
    while(cur_idx < str.size()){
        string doc_judgment_pair;
        while(cur_idx < str.size() && str[cur_idx] != ','){
            doc_judgment_pair.push_back(str[cur_idx]);
            cur_idx++;
        }
        cur_idx++;
        if(doc_judgment_pair.find(':') == string::npos){
            return false;
        }
        try{
            auto sep = doc_judgment_pair.find(':');
            seed_judgments.push_back(
                    {doc_judgment_pair.substr(0, sep), stoi(doc_judgment_pair.substr(sep+1))}
            );
        } catch (const invalid_argument& ia){
            return false;
        }
    }
    return true;
}

// Handler for API endpoint /begin
void begin_session_view(const FCGX_Request & request, const vector<pair<string, string>> &params){
    string session_id, query, mode = "doc";
    vector<pair<string, int>> seed_judgments;
    int judgments_per_iteration = -1;
    bool async_mode = false;

    for(auto kv: params){
        if(kv.first == "session_id"){
            session_id = kv.second;
        }else if(kv.first == "seed_query"){
            query = kv.second;
        }else if(kv.first == "mode"){
            mode = kv.second;
        }else if(kv.first == "seed_judgments"){
            if(!parse_seed_judgments(kv.second, seed_judgments)){
                write_response(request, 400, "application/json", "{\"error\": \"Invalid format for seed_judgments\"}");
                return;
            }
        }else if(kv.first == "judgments_per_iteration"){
            try {
                judgments_per_iteration = stoi(kv.second);
            } catch (const invalid_argument& ia) {
                write_response(request, 400, "application/json", "{\"error\": \"Invalid judgments_per_iteration\"}");
                return;
            }
        }else if(kv.first == "async"){
            if(kv.second == "true" || kv.second == "True"){
                async_mode = true;
            }else if(kv.second == "false" || kv.second == "False"){
                async_mode = false;
            }
        }
    }

    if(SESSIONS.find(session_id) != SESSIONS.end()){
        write_response(request, 400, "application/json", "{\"error\": \"session already exists\"}");
        return;
    }

    if(session_id.size() == 0 || query.size() == 0){
        write_response(request, 400, "application/json", "{\"error\": \"Non empty session_id and query required\"}");
        return;
    }

    Seed seed_query = {{features::get_features(query, *documents.get()), 1}};

    if(mode == "doc"){
        SESSIONS[session_id] = make_unique<BMI>(
                seed_query,
                documents.get(),
                CMD_LINE_INTS["--threads"],
                judgments_per_iteration,
                async_mode,
                200000);
    }else if(mode == "para"){
        SESSIONS[session_id] = make_unique<BMI_para>(
                seed_query,
                documents.get(),
                paragraphs.get(),
                CMD_LINE_INTS["--threads"],
                judgments_per_iteration,
                async_mode,
                200000);
    }else if(mode == "para_scal"){
        SESSIONS[session_id] = make_unique<BMI_para_scal>(
                seed_query,
                documents.get(),
                paragraphs.get(),
                CMD_LINE_INTS["--threads"],
                200000, 25, seed_judgments);
    }else if(mode == "doc_scal"){
        SESSIONS[session_id] = make_unique<BMI_doc_scal>(
                seed_query,
                documents.get(),
                CMD_LINE_INTS["--threads"],
                200000, 25, seed_judgments);
    }else {
        write_response(request, 400, "application/json", "{\"error\": \"Invalid mode\"}");
        return;
    }

    if(mode != "para_scal"){
        SESSIONS[session_id]->record_judgment_batch(seed_judgments);
        SESSIONS[session_id]->perform_training_iteration();
    }

    // need proper json parsing!!
    write_response(request, 200, "application/json", "{\"session-id\": \""+session_id+"\"}");
}

string stringify_top_terms(vector<pair<uint32_t, float>> top_terms) {
    string top_terms_json = "{";
    for(auto top_term: top_terms){
        if(top_terms_json.length() > 1)
            top_terms_json.push_back(',');
        top_terms_json += "\"" + id_tokens[top_term.first] + "\"" + ":" + to_string(top_term.second);
    }
    top_terms_json.push_back('}');
    return top_terms_json;   
}

string stringify_test_scores(vector<pair<string, float>> scores, unordered_map<string, int> labels) {
    string test_set_json = "{";
    for(auto doc_score: scores){
        if(doc_score.first[0] == '.') continue;
        if(test_set_json.length() > 1)
            test_set_json.push_back(',');
        test_set_json += "\"" + doc_score.first + "\"" + ":" + "{\"label\":" + to_string(test_labels_map[doc_score.first]) + ",\"score\":" + to_string(doc_score.second) + "}";
    }
    test_set_json.push_back('}');
    return test_set_json;
}

string stringify_top_model_terms(vector<float> weights, int num_top_terms = 10) {
    vector< pair<uint32_t, float> > id_weights;
    for(int i = 0; i < weights.size(); i++) {
        id_weights.push_back({i, weights[i]});
    }
    sort(id_weights.begin(), id_weights.end(), [](auto &left, auto &right) {
        return left.second > right.second;
    });

    string top_model_terms_json = "{";
    int num_to_take = num_top_terms < weights.size() ? num_top_terms : weights.size();
    for(int i = 0; i < num_top_terms; i++) {
        if(top_model_terms_json.length() > 1)
            top_model_terms_json.push_back(',');
        top_model_terms_json += "\"" + id_tokens[id_weights[i].first] + "\"" + ":" + to_string(id_weights[i].second);
    }
    top_model_terms_json.push_back('}');
    return top_model_terms_json;
}

// Fetch doc-ids in JSON
// Also fetches top terms
// Also fetches performance on test set
string get_docs(string session_id, int max_count, int num_top_terms = 10){
    const unique_ptr<BMI> &bmi = SESSIONS[session_id];
    vector<pair<string, float>> doc_ids = bmi->get_doc_to_judge(max_count);

    string doc_json = "[";
    string top_terms_json = "{";
    for(auto doc_id: doc_ids){
        if(doc_json.length() > 1)
            doc_json.push_back(',');
        if(top_terms_json.length() > 1)
            top_terms_json.push_back(',');
        doc_json += "{\"doc_id\":\"" + doc_id.first + "\",\"score\":" + to_string(doc_id.second) + "}";
        vector<pair<uint32_t, float>> top_terms = bmi->get_top_terms(doc_id.first, num_top_terms);
        cerr << stringify_top_terms(top_terms) << endl;
        top_terms_json += "\"" + doc_id.first + "\": " + stringify_top_terms(top_terms);
    }
    doc_json.push_back(']');
    top_terms_json.push_back('}');

    // Scoring of test docs
    auto state = bmi->get_state();
    auto weights = state.weights;
    auto model_no = state.cur_iteration;
    vector<pair<string, float>> test_docs_scores;
    for (int idx; idx < test_documents->size(); idx++) {
        float score = test_documents->inner_product(idx, weights);
        test_docs_scores.push_back({test_documents->get_id(idx), score});
        //cerr << test_documents->get_id(idx) << " " << score << endl;
    }
    unordered_map<string, int> labels;
    string test_set_json = stringify_test_scores(test_docs_scores, labels);

    string top_model_terms_json = stringify_top_model_terms(weights);

    return "{\"session-id\": \"" + session_id + "\", \"docs\": " + doc_json + ",\"top-terms\": " + top_terms_json + ",\"top-model-terms\": " + top_model_terms_json + ",\"test-scores\": " + test_set_json + ",\"model-number\": " + to_string(model_no) + "}";
}

// Handler for /delete_session
void delete_session_view(const FCGX_Request & request, const vector<pair<string, string>> &params){
    string session_id;

    for(auto kv: params){
        if(kv.first == "session_id"){
            session_id = kv.second;
        }
    }

    if(SESSIONS.find(session_id) == SESSIONS.end()){
        write_response(request, 404, "application/json", "{\"error\": \"session not found\"}");
        return;
    }

    SESSIONS.erase(session_id);

    write_response(request, 200, "application/json", "{\"session-id\": \"" + session_id + "\"}");
}

// Handler for /get_docs
void get_docs_view(const FCGX_Request & request, const vector<pair<string, string>> &params){
    string session_id;
    int max_count = 2;

    for(auto kv: params){
        if(kv.first == "session_id"){
            session_id = kv.second;
        }else if(kv.first == "max_count"){
            max_count = stoi(kv.second);
        }
    }

    if(session_id.size() == 0){
        write_response(request, 400, "application/json", "{\"error\": \"Non empty session_id required\"}");
        return;
    }

    if(SESSIONS.find(session_id) == SESSIONS.end()){
        write_response(request, 404, "application/json", "{\"error\": \"session not found\"}");
        return;
    }

    write_response(request, 200, "application/json", get_docs(session_id, max_count));
}

// Handler for /get_ranklist
void get_ranklist(const FCGX_Request & request, const vector<pair<string, string>> &params){
    string session_id;

    for(auto kv: params){
        if(kv.first == "session_id"){
            session_id = kv.second;
        }
    }

    if(session_id.size() == 0){
        write_response(request, 400, "application/json", "{\"error\": \"Non empty session_id required\"}");
    }

    if(SESSIONS.find(session_id) == SESSIONS.end()){
        write_response(request, 404, "application/json", "{\"error\": \"session not found\"}");
        return;
    }

    vector<pair<string, float>> ranklist = SESSIONS[session_id]->get_ranklist();

    string ranklist_str = "";
    for(int i=0; i < ranklist.size(); i++){
      ranklist_str += ranklist[i].first + " " + to_string(ranklist[i].second);
      if(i!=(ranklist.size()-1)){
        ranklist_str += ",";
      }
    }

    //text to json
    string json_ret = "{\"ranklist\": \"" + ranklist_str + "\"}";
    write_response(request, 200, "application/json", json_ret);
}

// Handler for /log
void log_view(const FCGX_Request & request, const vector<pair<string, string>> &params){
    string session_id;

    for(auto kv: params){
        if(kv.first == "session_id"){
            session_id = kv.second;
        }
    }

    if(session_id.size() == 0){
        write_response(request, 400, "application/json", "{\"error\": \"Non empty session_id required\"}");
    }

    if(SESSIONS.find(session_id) == SESSIONS.end()){
        write_response(request, 404, "application/json", "{\"error\": \"session not found\"}");
        return;
    }

    write_response(request, 200, "text/plain", SESSIONS[session_id]->get_log());
}

// Handler for /docid_exists
void docid_exists_view(const FCGX_Request & request, const vector<pair<string, string>> &params){
    string session_id, doc_id;

    for(auto kv: params){
        if(kv.first == "session_id"){
            session_id = kv.second;
        }else if(kv.first == "doc_id"){
            doc_id = kv.second;
        }
    }

    if(SESSIONS.find(session_id) == SESSIONS.end()){
        write_response(request, 404, "application/json", "{\"error\": \"session not found\"}");
        return;
    }

    const unique_ptr<BMI> &bmi = SESSIONS[session_id];

    if(bmi->get_dataset()->get_index(doc_id) == bmi->get_dataset()->NPOS){
        write_response(request, 200, "application/json", "{\"exists\": false}");
        return;
    }
    write_response(request, 200, "application/json", "{\"exists\": true}");
}

// Handler for /judge
void judge_view(const FCGX_Request & request, const vector<pair<string, string>> &params){
    string session_id, doc_id;
    int rel = -2;

    for(auto kv: params){
        if(kv.first == "session_id"){
            session_id = kv.second;
        }else if(kv.first == "doc_id"){
            doc_id = kv.second;
        }else if(kv.first == "rel"){
            rel = stoi(kv.second);
        }
    }

    if(session_id.size() == 0 || doc_id.size() == 0){
        write_response(request, 400, "application/json", "{\"error\": \"Non empty session_id and doc_id required\"}");
    }

    if(SESSIONS.find(session_id) == SESSIONS.end()){
        write_response(request, 404, "application/json", "{\"error\": \"session not found\"}");
        return;
    }

    const unique_ptr<BMI> &bmi = SESSIONS[session_id];
    if(bmi->get_dataset()->get_index(doc_id) == bmi->get_dataset()->NPOS){
        write_response(request, 404, "application/json", "{\"error\": \"doc_id not found\"}");
        return;
    }

    if(rel < -1 || rel > 1){
        write_response(request, 400, "application/json", "{\"error\": \"rel can either be -1, 0 or 1\"}");
        return;
    }

    bmi->record_judgment(doc_id, rel);
    write_response(request, 200, "application/json", get_docs(session_id, 20));
}

void log_request(const FCGX_Request & request, const vector<pair<string, string>> &params){
    cerr<<string(FCGX_GetParam("RELATIVE_URI", request.envp))<<endl;
    cerr<<FCGX_GetParam("REQUEST_METHOD", request.envp)<<endl;
    for(auto kv: params){
        cerr<<kv.first<<" "<<kv.second<<endl;
    }
    cerr<<endl;
}

void process_request(const FCGX_Request & request) {
    string action = parse_action_from_uri(FCGX_GetParam("RELATIVE_URI", request.envp));
    string method = FCGX_GetParam("REQUEST_METHOD", request.envp);

    vector<pair<string, string>> params;

    if(method == "GET")
        params = parse_query_string(FCGX_GetParam("QUERY_STRING", request.envp));
    else if(method == "POST" || method == "DELETE")
        params = parse_query_string(read_content(request));

    log_request(request, params);

    if(action == "begin"){
        if(method == "POST"){
            begin_session_view(request, params);
        }
    }else if(action == "get_docs"){
        if(method == "GET"){
            get_docs_view(request, params);
        }
    }else if(action == "docid_exists"){
        if(method == "GET"){
            docid_exists_view(request, params);
        }
    }else if(action == "judge"){
        if(method == "POST"){
            judge_view(request, params);
        }
    }else if(action == "get_ranklist"){
        if(method == "GET"){
            get_ranklist(request, params);
        }
    }else if(action == "delete_session"){
        if(method == "DELETE"){
            delete_session_view(request, params);
        }
    }else if(action == "log"){
        if(method == "GET"){
            log_view(request, params);
        }
    }
}

void fcgi_listener(){
    FCGX_Request request;
    FCGX_InitRequest(&request, 0, 0);

    while (FCGX_Accept_r(&request) == 0) {
        process_request(request);
    }
}

int main(int argc, char **argv){
    AddFlag("--doc-features", "Path of the file with list of document features", string(""));
    AddFlag("--test-features", "Path of the file with list of test document features", string(""));
    AddFlag("--test-labels", "Path of the file with list of test document labels", string(""));
    AddFlag("--para-features", "Path of the file with list of paragraph features", string(""));
    AddFlag("--df", "Path of the file with list of terms and their document frequencies", string(""));
    AddFlag("--threads", "Number of threads to use for scoring", int(8));
    AddFlag("--help", "Show Help", bool(false));

    ParseFlags(argc, argv);

    if(CMD_LINE_BOOLS["--help"]){
        ShowHelp();
        return 0;
    }

    if(CMD_LINE_STRINGS["--doc-features"].length() == 0){
        cerr<<"Required argument --doc-features missing"<<endl;
        return -1;
    }

    // Load docs
    TIMER_BEGIN(documents_loader);
    cerr<<"Loading document features on memory"<<endl;
    {
        unique_ptr<FeatureParser> feature_parser;
        if(CMD_LINE_STRINGS["--df"].size() > 0)
            feature_parser = make_unique<BinFeatureParser>(CMD_LINE_STRINGS["--doc-features"], CMD_LINE_STRINGS["--df"]);
        else
            feature_parser = make_unique<BinFeatureParser>(CMD_LINE_STRINGS["--doc-features"]);
        documents = Dataset::build(feature_parser.get());
        cerr<<"Read "<<documents->size()<<" docs"<<endl;
    }
    TIMER_END(documents_loader);

    // Load test docs
    cerr<<"Loading test document features on memory"<<endl;
    {
        unique_ptr<FeatureParser> feature_parser;
        feature_parser = make_unique<BinFeatureParser>(CMD_LINE_STRINGS["--test-features"]);
        test_documents = Dataset::build(feature_parser.get());
        cerr<<"Read "<<test_documents->size()<<" test docs"<<endl;
    }

    // Load para
    string para_features_path = CMD_LINE_STRINGS["--para-features"];
    if(para_features_path.length() > 0){
        TIMER_BEGIN(paragraph_loader);
        cerr<<"Loading paragraph features on memory"<<endl;
        {
            unique_ptr<FeatureParser> feature_parser;
            if(CMD_LINE_STRINGS["--df"].size() > 0)
                feature_parser = make_unique<BinFeatureParser>(para_features_path, "");
            else
                feature_parser = make_unique<BinFeatureParser>(para_features_path);
            paragraphs = ParagraphDataset::build(feature_parser.get(), *documents);
            cerr<<"Read "<<paragraphs->size()<<" paragraphs"<<endl;
        }
        TIMER_END(paragraph_loader);
    }

    string id_term_map_path = "/data/id_token_map.txt";
    if(id_term_map_path.length() > 0){
        cerr<<"Loading id-tokens map on memory"<<endl;
        {
            ifstream id_map_file;
            id_map_file.open(id_term_map_path);
            string line;
            while (getline(id_map_file, line)){
                istringstream iss(line);
                uint32_t token_id;
                string token;
                if (!(iss >> token_id >> token)) { break; } // error
                id_tokens[token_id] = token;
            }
            id_map_file.close();
            cerr<<"Read "<< id_tokens.size() << " id-tokens entries"<<endl;
        }
    }

    string test_labels_path = CMD_LINE_STRINGS["--test-labels"];
    if(test_labels_path.length() > 0){
        cerr<<"Loading labels file"<<endl;
        {
            ifstream test_labels_file;
            test_labels_file.open(test_labels_path);
            string line;
            while (getline(test_labels_file, line)){
                istringstream iss(line);
                string doc_id;
                int label;
                if (!(iss >> doc_id >> label)) { break; } // error
                test_labels_map[doc_id] = label;
            }
            test_labels_file.close();
            cerr<<"Read "<< test_labels_map.size() << " test file labels"<<endl;
        }
    }

    FCGX_Init();

    vector<thread> fastcgi_threads;
    for(int i = 0;i < 50; i++){
        fastcgi_threads.push_back(thread(fcgi_listener));
    }

    for(auto &t: fastcgi_threads){
        t.join();
    }

    return 0;
}
