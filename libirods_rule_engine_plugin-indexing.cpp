
// =-=-=-=-=-=-=-
// irods includes
#include "irods_re_plugin.hpp"
#include "irods_re_ruleexistshelper.hpp"
#include "irods_hierarchy_parser.hpp"
#include "irods_resource_backport.hpp"
#include "irods_query.hpp"
#include "rsModAVUMetadata.hpp"

#include "utilities.hpp"
#include "indexing_utilities.hpp"

#undef LIST

// =-=-=-=-=-=-=-
// stl includes
#include <iostream>
#include <sstream>
#include <vector>
#include <string>

// =-=-=-=-=-=-=-
// boost includes
#include <boost/any.hpp>
#include <boost/exception/all.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/optional.hpp>

#include "json.hpp"

#include "objDesc.hpp"
extern l1desc_t L1desc[NUM_L1_DESC];

int _delayExec(
    const char *inActionCall,
    const char *recoveryActionCall,
    const char *delayCondition,
    ruleExecInfo_t *rei );

namespace {
    bool collection_metadata_is_new = false;
    std::unique_ptr<irods::indexing::configuration>     config;
    std::map<int, std::tuple<std::string, std::string>> opened_objects;

    std::tuple<int, std::string>
    get_index_and_resource(const dataObjInp_t* _inp) {
        int l1_idx{};
        dataObjInfo_t* obj_info{};
        for(const auto& l1 : L1desc) {
            if(FD_INUSE != l1.inuseFlag) {
                continue;
            }
            if(!strcmp(l1.dataObjInp->objPath, _inp->objPath)) {
                obj_info = l1.dataObjInfo;
                l1_idx = &l1 - L1desc;
            }
        }

        if(nullptr == obj_info) {
            THROW(
                SYS_INVALID_INPUT_PARAM,
                "no object found");
        }

        std::string resource_name;
        irods::error err = irods::get_resource_property<std::string>(
                               obj_info->rescId,
                               irods::RESOURCE_NAME,
                               resource_name);
        if(!err.ok()) {
            THROW(err.code(), err.result());
        }

        return std::make_tuple(l1_idx, resource_name);
    } // get_object_path_and_resource

    void apply_indexing_policy(
        const std::string &    _rn,
        ruleExecInfo_t*        _rei,
        std::list<boost::any>& _args) {
        try {
            std::string object_path;
            std::string source_resource;
            // NOTE:: 3rd parameter is the target
            if("pep_api_data_obj_put_post"  == _rn) {
                auto it = _args.begin();
                std::advance(it, 2);
                if(_args.end() == it) {
                    THROW(
                        SYS_INVALID_INPUT_PARAM,
                        "invalid number of arguments");
                }

                auto obj_inp = boost::any_cast<dataObjInp_t*>(*it);
                object_path = obj_inp->objPath;
                const char* resc_hier = getValByKey(
                                            &obj_inp->condInput,
                                            RESC_HIER_STR_KW);
                if(!resc_hier) {
                    THROW(SYS_INVALID_INPUT_PARAM, "resc hier is null");
                }

                irods::hierarchy_parser parser;
                parser.set_string(resc_hier);
                parser.last_resc(source_resource);

                irods::indexing::indexer idx{_rei, config->instance_name_};
                idx.schedule_full_text_indexing_event(
                    object_path,
                    _rei->rsComm->clientUser.userName,
                    source_resource);
            }
            else if("pep_api_data_obj_repl_post" == _rn) {
                auto it = _args.begin();
                std::advance(it, 2);
                if(_args.end() == it) {
                    THROW(
                        SYS_INVALID_INPUT_PARAM,
                        "invalid number of arguments");
                }

                auto obj_inp = boost::any_cast<dataObjInp_t*>(*it);
                object_path = obj_inp->objPath;
                const char* resc_hier = getValByKey(
                                            &obj_inp->condInput,
                                            DEST_RESC_HIER_STR_KW);
                if(!resc_hier) {
                    THROW(SYS_INVALID_INPUT_PARAM, "resc hier is null");
                }

                irods::hierarchy_parser parser;
                parser.set_string(resc_hier);
                parser.last_resc(source_resource);

                irods::indexing::indexer idx{_rei, config->instance_name_};
                idx.schedule_full_text_indexing_event(
                    object_path,
                    _rei->rsComm->clientUser.userName,
                    source_resource);
            }
            else if("pep_api_data_obj_open_post"   == _rn ||
                    "pep_api_data_obj_create_post" == _rn) {
                auto it = _args.begin();
                std::advance(it, 2);
                if(_args.end() == it) {
                    THROW(
                        SYS_INVALID_INPUT_PARAM,
                        "invalid number of arguments");
                }

                auto obj_inp = boost::any_cast<dataObjInp_t*>(*it);
                if(obj_inp->openFlags & O_WRONLY || obj_inp->openFlags & O_RDWR) {
                    int l1_idx{};
                    std::string resource_name;
                    try {
                        std::tie(l1_idx, resource_name) = get_index_and_resource(obj_inp);
                        opened_objects[l1_idx] = std::tie(obj_inp->objPath, resource_name);
                    }
                    catch(const irods::exception& _e) {
                        rodsLog(
                           LOG_ERROR,
                           "get_index_and_resource failed for [%s]",
                           obj_inp->objPath);
                    }
                }
            }
            else if("pep_api_data_obj_close_post" == _rn) {
                //TODO :: only for create/write events
                auto it = _args.begin();
                std::advance(it, 2);
                if(_args.end() == it) {
                    THROW(
                        SYS_INVALID_INPUT_PARAM,
                        "invalid number of arguments");
                }

                const auto opened_inp = boost::any_cast<openedDataObjInp_t*>(*it);
                const auto l1_idx = opened_inp->l1descInx;
                if(opened_objects.find(l1_idx) != opened_objects.end()) {
                    std::string object_path, resource_name;
                    std::tie(object_path, resource_name) = opened_objects[l1_idx];
                    irods::indexing::indexer idx{_rei, config->instance_name_};
                    idx.schedule_full_text_indexing_event(
                        object_path,
                        _rei->rsComm->clientUser.userName,
                        resource_name);
                }
            }
            else if("pep_api_mod_avu_metadata_pre" == _rn) {
                auto it = _args.begin();
                std::advance(it, 2);
                if(_args.end() == it) {
                    THROW(
                        SYS_INVALID_INPUT_PARAM,
                        "invalid number of arguments");
                }

                const auto avu_inp = boost::any_cast<modAVUMetadataInp_t*>(*it);
                const std::string attribute{avu_inp->arg3};
                if(config->index != attribute) {
                    return;
                }

                const std::string operation{avu_inp->arg0};
                const std::string type{avu_inp->arg1};
                const std::string object_path{avu_inp->arg2};
                const std::string add{"add"};
                const std::string set{"set"};
                const std::string collection{"-C"};

                irods::indexing::indexer idx{_rei, config->instance_name_};
                if(operation == set || operation == add) {
                    if(type == collection) {
                        // was the added tag an indexing indicator
                        if(config->index == attribute) {
                            // verify that this is not new metadata with a query and set a flag
                            collection_metadata_is_new = !idx.metadata_exists_on_collection(
                                                             object_path,
                                                             avu_inp->arg3,
                                                             avu_inp->arg4,
                                                             avu_inp->arg5);
                        }
                    }
                }
            }
            else if("pep_api_mod_avu_metadata_post" == _rn) {
                auto it = _args.begin();
                std::advance(it, 2);
                if(_args.end() == it) {
                    THROW(
                        SYS_INVALID_INPUT_PARAM,
                        "invalid number of arguments");
                }

                const auto avu_inp = boost::any_cast<modAVUMetadataInp_t*>(*it);
                const std::string operation{avu_inp->arg0};
                const std::string type{avu_inp->arg1};
                const std::string collection_name{avu_inp->arg2};
                const std::string attribute{avu_inp->arg3};
                const std::string value{avu_inp->arg4};
                const std::string units{avu_inp->arg5};
                const std::string add{"add"};
                const std::string set{"set"};
                const std::string rm{"rm"};
                const std::string collection{"-C"};
                const std::string data_object{"-d"};

                irods::indexing::indexer idx{_rei, config->instance_name_};
                if(operation == rm) {
                    // removed index metadata from collection
                    if(type == collection) {
                        // was the removed tag an indexing indicator
                        if(config->index == attribute) {
                            // schedule a possible purge of all indexed data in collection
                            idx.schedule_collection_operation(
                                irods::indexing::operation_type::purge,
                                collection_name,
                                _rei->rsComm->clientUser.userName,
                                value,
                                units);
                        }
                    }
                    // removed a single indexed AVU on an object
                    if(type == data_object) {
                        // schedule an AVU purge
                        idx.schedule_metadata_purge_event(
                                collection_name,
                                _rei->rsComm->clientUser.userName,
                                attribute,
                                value,
                                units);
                    }
                }
                else if(operation == set || operation == add) {
                    if(type == collection) {
                        // was the added tag an indexing indicator
                        if(config->index == attribute) {
                            // check the verify flag
                            if(collection_metadata_is_new) {
                                idx.schedule_collection_operation(
                                    irods::indexing::operation_type::index,
                                    collection_name,
                                    _rei->rsComm->clientUser.userName,
                                    value,
                                    units);
                            }
                        }
                    }
                    if(type == data_object) {
                        idx.schedule_metadata_indexing_event(
                                collection_name,
                                _rei->rsComm->clientUser.userName,
                                attribute,
                                value,
                                units);
                    }
                }
            }
            else if("pep_api_data_obj_unlink_post" == _rn) {
                auto it = _args.begin();
                std::advance(it, 2);
                if(_args.end() == it) {
                    THROW(
                        SYS_INVALID_INPUT_PARAM,
                        "invalid number of arguments");
                }

                const auto obj_inp = boost::any_cast<dataObjInp_t*>(*it);
                irods::indexing::indexer idx{_rei, config->instance_name_};
                idx.schedule_full_text_purge_event(
                    obj_inp->objPath,
                    _rei->rsComm->clientUser.userName);
            }
        }
        catch(const boost::bad_any_cast& _e) {
            THROW(
                INVALID_ANY_CAST,
                boost::str(boost::format(
                    "function [%s] rule name [%s]")
                    % __FUNCTION__ % _rn));
        }
    } // apply_indexing_policy

    void apply_object_policy(
        ruleExecInfo_t*    _rei,
        const std::string& _policy_root,
        const std::string& _object_path,
        const std::string& _source_resource,
        const std::string& _indexer,
        const std::string& _index_name,
        const std::string& _index_type) {
        const std::string policy_name{irods::indexing::policy::compose_policy_name(
                              _policy_root,
                              _indexer)};

        std::list<boost::any> args;
        args.push_back(boost::any(_object_path));
        args.push_back(boost::any(_source_resource));
        args.push_back(boost::any(_index_name));
        args.push_back(boost::any(_index_type));
        irods::indexing::invoke_policy(_rei, policy_name, args);

    } // apply_object_policy

    void apply_metadata_policy(
        ruleExecInfo_t*    _rei,
        const std::string& _policy_root,
        const std::string& _object_path,
        const std::string& _indexer,
        const std::string& _index_name,
        const std::string& _attribute,
        const std::string& _value,
        const std::string& _units) {
        const std::string policy_name{irods::indexing::policy::compose_policy_name(
                              _policy_root,
                              _indexer)};

        if(_attribute.empty() && _value.empty()) {
            const boost::filesystem::path p{_object_path};
            const std::string coll_name = p.parent_path().string();
            const std::string data_name = p.filename().string();
            const std::string query_str {
                boost::str(
                    boost::format("SELECT META_DATA_ATTR_NAME, META_DATA_ATTR_VALUE, META_DATA_ATTR_UNITS WHERE DATA_NAME = '%s' AND COLL_NAME = '%s'")
                        % data_name
                        % coll_name) };
            irods::query<rsComm_t> qobj{_rei->rsComm, query_str};
            for (const auto& result : qobj) {
                std::list<boost::any> args;
                args.push_back(boost::any(_object_path));
                args.push_back(boost::any(result[0]));
                args.push_back(boost::any(result[1]));
                args.push_back(boost::any(result[2]));
                args.push_back(boost::any(_index_name));
                irods::indexing::invoke_policy(_rei, policy_name, args);
            } // for
        }
        else {
            std::list<boost::any> args;
            args.push_back(boost::any(_object_path));
            args.push_back(boost::any(_attribute));
            args.push_back(boost::any(_value));
            args.push_back(boost::any(_units));
            args.push_back(boost::any(_index_name));
            irods::indexing::invoke_policy(_rei, policy_name, args);
        }
    } // apply_metadata_policy

} // namespace


irods::error start(
    irods::default_re_ctx&,
    const std::string& _instance_name ) {
    RuleExistsHelper::Instance()->registerRuleRegex("pep_api_.*");
    config = std::make_unique<irods::indexing::configuration>(_instance_name);
    return SUCCESS();
} // start

irods::error stop(
    irods::default_re_ctx&,
    const std::string& ) {
    return SUCCESS();
} // stop

irods::error rule_exists(
    irods::default_re_ctx&,
    const std::string& _rn,
    bool&              _ret) {
    const std::set<std::string> rules{
                                    "pep_api_data_obj_open_post",
                                    "pep_api_data_obj_create_post",
                                    "pep_api_data_obj_repl_post",
                                    "pep_api_data_obj_unlink_post",
                                    "pep_api_mod_avu_metadata_pre",
                                    "pep_api_mod_avu_metadata_post",
                                    "pep_api_data_obj_close_post",
                                    "pep_api_data_obj_put_post",
                                    "pep_api_phy_path_reg_post"};
    _ret = rules.find(_rn) != rules.end();

    return SUCCESS();
} // rule_exists

irods::error list_rules(irods::default_re_ctx&, std::vector<std::string>&) {
    return SUCCESS();
} // list_rules

irods::error exec_rule(
    irods::default_re_ctx&,
    const std::string&     _rn,
    std::list<boost::any>& _args,
    irods::callback        _eff_hdlr) {
    ruleExecInfo_t* rei{};
    const auto err = _eff_hdlr("unsafe_ms_ctx", &rei);
    if(!err.ok()) {
        return err;
    }
    try {
        apply_indexing_policy(_rn, rei, _args);
    }
    catch(const  std::invalid_argument& _e) {
        irods::indexing::exception_to_rerror(
            SYS_NOT_SUPPORTED,
            _e.what(),
            rei->rsComm->rError);
        return ERROR(
                   SYS_NOT_SUPPORTED,
                   _e.what());
    }
    catch(const std::domain_error& _e) {
        irods::indexing::exception_to_rerror(
            INVALID_ANY_CAST,
            _e.what(),
            rei->rsComm->rError);
        return ERROR(
                   SYS_NOT_SUPPORTED,
                   _e.what());
    }
    catch(const irods::exception& _e) {
        irods::indexing::exception_to_rerror(
            _e,
            rei->rsComm->rError);
        return irods::error(_e);
    }

    return err;

} // exec_rule

irods::error exec_rule_text(
    irods::default_re_ctx&,
    const std::string&  _rule_text,
    msParamArray_t*     _ms_params,
    const std::string&  _out_desc,
    irods::callback     _eff_hdlr) {
    using json = nlohmann::json;

    try {
        // skip the first line: @external
        std::string rule_text{_rule_text};
        if(_rule_text.find("@external") != std::string::npos) {
            rule_text = _rule_text.substr(10);
        }
        const auto rule_obj = json::parse(rule_text);
        const std::string& rule_engine_instance_name = rule_obj["rule-engine-instance-name"];
        // if the rule text does not have our instance name, fail
        if(config->instance_name_ != rule_engine_instance_name) {
            return ERROR(
                    SYS_NOT_SUPPORTED,
                    "instance name not found");
        }
#if 0
        // catalog / index drift correction
        if(irods::indexing::schedule::indexing ==
           rule_obj["rule-engine-operation"]) {
            ruleExecInfo_t* rei{};
            const auto err = _eff_hdlr("unsafe_ms_ctx", &rei);
            if(!err.ok()) {
                return err;
            }

            const std::string& params = rule_obj["delay-parameters"];

            json delay_obj;
            delay_obj["rule-engine-operation"] = irods::indexing::policy::indexing;

            irods::indexing::indexer idx{rei, config->instance_name_};
            idx.schedule_indexing_policy(
                delay_obj.dump(),
                params);
        }
        else
#endif
        {
            return ERROR(
                    SYS_NOT_SUPPORTED,
                    "supported rule name not found");
        }
    }
    catch(const  std::invalid_argument& _e) {
        std::string msg{"Rule text is not valid JSON -- "};
        msg += _e.what();
        return ERROR(
                   SYS_NOT_SUPPORTED,
                   msg);
    }
    catch(const std::domain_error& _e) {
        std::string msg{"Rule text is not valid JSON -- "};
        msg += _e.what();
        return ERROR(
                   SYS_NOT_SUPPORTED,
                   msg);
    }
    catch(const irods::exception& _e) {
        return ERROR(
                _e.code(),
                _e.what());
    }

    return SUCCESS();
} // exec_rule_text

irods::error exec_rule_expression(
    irods::default_re_ctx&,
    const std::string&     _rule_text,
    msParamArray_t*        _ms_params,
    irods::callback        _eff_hdlr) {
    using json = nlohmann::json;
    ruleExecInfo_t* rei{};
    const auto err = _eff_hdlr("unsafe_ms_ctx", &rei);
    if(!err.ok()) {
        return err;
    }

    try {
        const auto rule_obj = json::parse(_rule_text);
        if(irods::indexing::policy::object::index ==
           rule_obj["rule-engine-operation"]) {
            try {
                // proxy for provided user name
                const std::string& user_name = rule_obj["user-name"];
                rstrcpy(
                    rei->rsComm->clientUser.userName,
                    user_name.c_str(),
                    NAME_LEN);

                apply_object_policy(
                    rei,
                    irods::indexing::policy::object::index,
                    rule_obj["object-path"],
                    rule_obj["source-resource"],
                    rule_obj["indexer"],
                    rule_obj["index-name"],
                    rule_obj["index-type"]);
            }
            catch(const irods::exception& _e) {
                printErrorStack(&rei->rsComm->rError);
                return ERROR(
                        _e.code(),
                        _e.what());
            }
        }
        else if(irods::indexing::policy::object::purge ==
                rule_obj["rule-engine-operation"]) {
            try {
                // proxy for provided user name
                const std::string& user_name = rule_obj["user-name"];
                rstrcpy(
                    rei->rsComm->clientUser.userName,
                    user_name.c_str(),
                    NAME_LEN);

                apply_object_policy(
                    rei,
                    irods::indexing::policy::object::purge,
                    rule_obj["object-path"],
                    rule_obj["source-resource"],
                    rule_obj["indexer"],
                    rule_obj["index-name"],
                    rule_obj["index-type"]);
            }
            catch(const irods::exception& _e) {
                printErrorStack(&rei->rsComm->rError);
                return ERROR(
                        _e.code(),
                        _e.what());
            }
        }
        else if(irods::indexing::policy::collection::index ==
                rule_obj["rule-engine-operation"]) {

            irods::indexing::indexer idx{rei, config->instance_name_};
            idx.schedule_policy_events_for_collection(
                irods::indexing::operation_type::index,
                rule_obj["collection-name"],
                rule_obj["user-name"],
                rule_obj["indexer"],
                rule_obj["index-name"],
                rule_obj["index-type"]);
        }
        else if(irods::indexing::policy::collection::purge ==
                rule_obj["rule-engine-operation"]) {

            irods::indexing::indexer idx{rei, config->instance_name_};
            idx.schedule_policy_events_for_collection(
                irods::indexing::operation_type::purge,
                rule_obj["collection-name"],
                rule_obj["user-name"],
                rule_obj["indexer"],
                rule_obj["index-name"],
                rule_obj["index-type"]);
        }
        else if(irods::indexing::policy::metadata::index ==
                rule_obj["rule-engine-operation"]) {
            try {
                // proxy for provided user name
                const std::string& user_name = rule_obj["user-name"];
                rstrcpy(
                    rei->rsComm->clientUser.userName,
                    user_name.c_str(),
                    NAME_LEN);

                apply_metadata_policy(
                    rei,
                    irods::indexing::policy::metadata::index,
                    rule_obj["object-path"],
                    rule_obj["indexer"],
                    rule_obj["index-name"],
                    rule_obj["attribute"],
                    rule_obj["value"],
                    rule_obj["units"]);
            }
            catch(const irods::exception& _e) {
                printErrorStack(&rei->rsComm->rError);
                return ERROR(
                        _e.code(),
                        _e.what());
            }
        }
        else if(irods::indexing::policy::metadata::purge ==
                rule_obj["rule-engine-operation"]) {
            try {
                // proxy for provided user name
                const std::string& user_name = rule_obj["user-name"];
                rstrcpy(
                    rei->rsComm->clientUser.userName,
                    user_name.c_str(),
                    NAME_LEN);

                apply_metadata_policy(
                    rei,
                    irods::indexing::policy::metadata::purge,
                    rule_obj["object-path"],
                    rule_obj["indexer"],
                    rule_obj["index-name"],
                    rule_obj["attribute"],
                    rule_obj["value"],
                    rule_obj["units"]);
            }
            catch(const irods::exception& _e) {
                printErrorStack(&rei->rsComm->rError);
                return ERROR(
                        _e.code(),
                        _e.what());
            }
        }
        else {
            printErrorStack(&rei->rsComm->rError);
            return ERROR(
                    SYS_NOT_SUPPORTED,
                    "supported rule name not found");
        }
    }
    catch(const  std::invalid_argument& _e) {
        return ERROR(
                   SYS_NOT_SUPPORTED,
                   _e.what());
    }
    catch(const std::domain_error& _e) {
        return ERROR(
                   SYS_NOT_SUPPORTED,
                   _e.what());
    }
    catch(const irods::exception& _e) {
        return ERROR(
                _e.code(),
                _e.what());
    }

    return SUCCESS();

} // exec_rule_expression

extern "C"
irods::pluggable_rule_engine<irods::default_re_ctx>* plugin_factory(
    const std::string& _inst_name,
    const std::string& _context ) {
    irods::pluggable_rule_engine<irods::default_re_ctx>* re = 
        new irods::pluggable_rule_engine<irods::default_re_ctx>(
                _inst_name,
                _context);

    re->add_operation<
        irods::default_re_ctx&,
        const std::string&>(
            "start",
            std::function<
                irods::error(
                    irods::default_re_ctx&,
                    const std::string&)>(start));

    re->add_operation<
        irods::default_re_ctx&,
        const std::string&>(
            "stop",
            std::function<
                irods::error(
                    irods::default_re_ctx&,
                    const std::string&)>(stop));

    re->add_operation<
        irods::default_re_ctx&,
        const std::string&,
        bool&>(
            "rule_exists",
            std::function<
                irods::error(
                    irods::default_re_ctx&,
                    const std::string&,
                    bool&)>(rule_exists));

    re->add_operation<
        irods::default_re_ctx&,
        std::vector<std::string>&>(
            "list_rules",
            std::function<
                irods::error(
                    irods::default_re_ctx&,
                    std::vector<std::string>&)>(list_rules));

    re->add_operation<
        irods::default_re_ctx&,
        const std::string&,
        std::list<boost::any>&,
        irods::callback>(
            "exec_rule",
            std::function<
                irods::error(
                    irods::default_re_ctx&,
                    const std::string&,
                    std::list<boost::any>&,
                    irods::callback)>(exec_rule));

    re->add_operation<
        irods::default_re_ctx&,
        const std::string&,
        msParamArray_t*,
        const std::string&,
        irods::callback>(
            "exec_rule_text",
            std::function<
                irods::error(
                    irods::default_re_ctx&,
                    const std::string&,
                    msParamArray_t*,
                    const std::string&,
                    irods::callback)>(exec_rule_text));

    re->add_operation<
        irods::default_re_ctx&,
        const std::string&,
        msParamArray_t*,
        irods::callback>(
            "exec_rule_expression",
            std::function<
                irods::error(
                    irods::default_re_ctx&,
                    const std::string&,
                    msParamArray_t*,
                    irods::callback)>(exec_rule_expression));
    return re;

} // plugin_factory

