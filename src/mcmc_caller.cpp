#include "mcmc_caller.hpp"  
#include "graph_caller.hpp"
#include "algorithms/expand_context.hpp"
#include "memoizing_graph.hpp"


namespace vg {

    /**
     * MCMCCaller : Inherits from VCFOutputCaller    
     */         
    MCMCCaller::MCMCCaller(const PathPositionHandleGraph& graph,
                           SnarlManager& snarl_manager,
                           const string& sample_name,
                           const vector<string>& ref_paths,
                           const vector<size_t>& ref_path_offsets,
                           const vector<size_t>& ref_path_lengths,
                           ostream& out_stream) :
        graph(graph), snarl_manager(snarl_manager), VCFOutputCaller(sample_name), sample_name(sample_name), ref_paths(ref_paths), ref_path_offsets(ref_path_offsets),
        ref_path_lengths(ref_path_lengths), out_stream(out_stream) {
        
        /// keep track of offsets in the reference paths
        // used to offset variant reference position
        for (int i = 0; i < ref_paths.size(); ++i) {
            ref_offsets[ref_paths[i]] = i < ref_path_offsets.size() ? ref_path_offsets[i] : 0;
        }

    }
    
    MCMCCaller:: ~MCMCCaller(){

    }


    void MCMCCaller::call_top_level_snarls(bool recurse_on_fail) {

    // Used to recurse on children of parents that can't be called
    vector<const Snarl*> snarl_queue;

    // Run the snarl caller on a snarl, and queue up the children if it fails
    auto process_snarl = [&](const Snarl* snarl) {
        bool was_called = call_snarl(*snarl);
        if (!was_called && recurse_on_fail) {
            const vector<const Snarl*>& children = snarl_manager.children_of(snarl);
#pragma omp critical (snarl_queue)
            {
                snarl_queue.insert(snarl_queue.end(), children.begin(), children.end());
            }
        }
    };

    // Start with the top level snarls
    snarl_manager.for_each_top_level_snarl_parallel(process_snarl);

    // Then recurse on any children the snarl caller failed to handle
    while (!snarl_queue.empty()) {
        vector<const Snarl*> cur_queue;
        std::swap(snarl_queue, cur_queue);
#pragma omp parallel for
        for (int i = 0; i < cur_queue.size(); ++i) {
            process_snarl(cur_queue[i]);
        }
    }
  
    }

    bool MCMCCaller::call_snarl(const Snarl& snarl){
         // if we can't handle the snarl, then the GraphCaller framework will recurse on its children
        if (!is_traversable(snarl)) {
            return false;
        }
        
        PathTraversalFinder trav_finder(graph, snarl_manager);
        
        auto trav_results = trav_finder.find_path_traversals(snarl);
        vector<SnarlTraversal> ref_path = trav_results.first;
        
        //If it can't find any traversals, you can't output the snarl In VCF.
        if(!ref_path.empty()){
            // continue the loop of snarl without printing VCF file
            return 0;
     
        }else{
            //In practice there should be only one reference path, 
            //so you can just choose the first one returned
            SnarlTraversal trav = ref_path[0];
            
            
            vector<pair<step_handle_t, step_handle_t> > steps = trav_results.second;
            //<start_step, end_step>
            pair<step_handle_t, step_handle_t> start_and_end_pair = steps[0];
            step_handle_t first_start_step = start_and_end_pair.first;


            string ref_path_name  = trav.name();;
            for (int i = 0; i < trav.visit_size(); ++i) {
                const Visit& visit = trav.visit(i);
                //get sequence of reference path by visiting each node

                

                if(visit.backward() == true){
                    //use reverse_compliment??
                }
            
            }

            //pair<size_t, bool> ref_pos;
            
            vector<SnarlTraversal> haplo_travs;

            
            vector<int> genotype;
            //emit variant
            emit_variant(snarl, genotype, ref_path_name, haplo_travs);
        
        }
        i++;
        return 1;
    
    }
    void MCMCCaller::emit_variant(const Snarl& snarl, const vector<int>& genotype, 
                                 const string& ref_path_name, const vector<SnarlTraversal>& haplo_travs) const{
        
        // convert traversal to string
        // function that converst SnarlTraversals to strings
        // usage: trav_string(SnarlTraversal)
        function<string(const SnarlTraversal&)> trav_string = [&](const SnarlTraversal& trav) {
            string seq;
            for (int i = 0; i < trav.visit_size(); ++i) {
                seq += graph.get_sequence(graph.get_handle(trav.visit(i).node_id(), trav.visit(i).backward()));
            }
            return seq;
        };

    
        vcflib::Variant out_variant;

        // when calling alt/alt, the reference traversal doesn't end up in called_traversals.
        // this should get changed, but in the meantime we add it back here (as we need it for
        // the VCF output)
        // udpate: the reference traversal will be there when re-genotyping, but we can leave this logic
        // in case we want to ever add an option to toggle this.
        vector<SnarlTraversal> site_traversals;
        vector<int> site_genotype;
        for (int i = 0; i < genotype.size(); ++i) {
            if (genotype[i] == 0) {
                site_traversals.push_back(haplo_travs[i]);
                break;
            }
        }
        
        out_variant.ref = trav_string(site_traversals[0]);
        
        // deduplicate alleles and compute the site traversals and genotype
        map<string, int> allele_to_gt;    
        allele_to_gt[out_variant.ref] = 0;    
        for (int i = 0; i < genotype.size(); ++i) {
            if (genotype[i] == 0) {
                site_genotype.push_back(0);
            } else {
                string allele_string = trav_string(haplo_travs[i]);
                if (allele_to_gt.count(allele_string)) {
                    site_genotype.push_back(allele_to_gt[allele_string]);
                } else {
                    site_traversals.push_back(haplo_travs[i]);
                    site_genotype.push_back(allele_to_gt.size());
                    allele_to_gt[allele_string] = site_genotype.back();
                }
            }
        }

        out_variant.alt.resize(allele_to_gt.size() - 1);
        out_variant.alleles.resize(allele_to_gt.size());
        for (auto& allele_gt : allele_to_gt) {
            if (allele_gt.second > 0) {
                out_variant.alt[allele_gt.second - 1] = allele_gt.first;
            }
            out_variant.alleles[allele_gt.second] = allele_gt.first;
        }

        // fill out the rest of the variant
        out_variant.sequenceName = ref_path_name;
        // +1 to convert to 1-based VCF
        //out_variant.position = graph.get_position_of_step(first_start_step);
        out_variant.position = get_ref_position(snarl, ref_path_name).first + ref_offsets.find(ref_path_name)->second + 1;
        out_variant.id = std::to_string(snarl.start().node_id()) + "_" + std::to_string(snarl.end().node_id());
        out_variant.filter = "PASS";
        //out_variant.updateAlleleIndexes();

        // add the genotype
        out_variant.format.push_back("GT");
        auto& genotype_vector = out_variant.samples[sample_name]["GT"];
        
        // stringstream vcf_gt;
        // for (int i = 0; i < site_genotype.size(); ++i) {
        //     vcf_gt << site_genotype[i];
        //     if (i != site_genotype.size() - 1) {
        //         vcf_gt << "/";
        //     }
        // }
        // genotype_vector.push_back(vcf_gt.str());
        
        // clean up the alleles to not have so man common prefixes
        flatten_common_allele_ends(out_variant, true);
        flatten_common_allele_ends(out_variant, false);

        // add variant to list 
        if (!out_variant.alt.empty()) {
            add_variant(out_variant);
        }

        //write to output_vcf after each variant processed
        //better implimentation in regards to memory usage
        out_variant.setVariantCallFile(output_vcf);
        out_stream << out_variant << endl;
    }

    pair<size_t, bool> MCMCCaller::get_ref_position(const Snarl& snarl, const string& ref_path_name) const {
        path_handle_t path_handle = graph.get_path_handle(ref_path_name);

        handle_t start_handle = graph.get_handle(snarl.start().node_id(), snarl.start().backward());
        map<size_t, step_handle_t> start_steps;
        graph.for_each_step_on_handle(start_handle, [&](step_handle_t step) {
                if (graph.get_path_handle_of_step(step) == path_handle) {
                    start_steps[graph.get_position_of_step(step)] = step;
                }
            });

        handle_t end_handle = graph.get_handle(snarl.end().node_id(), snarl.end().backward());
        map<size_t, step_handle_t> end_steps;
        graph.for_each_step_on_handle(end_handle, [&](step_handle_t step) {
                if (graph.get_path_handle_of_step(step) == path_handle) {
                    end_steps[graph.get_position_of_step(step)] = step;
                }
            });

        assert(start_steps.size() > 0 && end_steps.size() > 0);
        step_handle_t start_step = start_steps.begin()->second;
        step_handle_t end_step = end_steps.begin()->second;
        bool scan_backward = graph.get_is_reverse(graph.get_handle_of_step(start_step));

        // if we're on a cycle, we keep our start step and find the end step by scanning the path
        if (start_steps.size() > 1 || end_steps.size() > 1) {
            bool found_end = false;
            if (scan_backward) {
                for (step_handle_t cur_step = start_step; graph.has_previous_step(end_step) && !found_end;
                    cur_step = graph.get_previous_step(cur_step)) {
                    if (graph.get_handle_of_step(cur_step) == end_handle) {
                        end_step = cur_step;
                        found_end = true;
                    }
                }
                assert(found_end);
            } else {
                for (step_handle_t cur_step = start_step; graph.has_next_step(end_step) && !found_end;
                    cur_step = graph.get_next_step(cur_step)) {
                    if (graph.get_handle_of_step(cur_step) == end_handle) {
                        end_step = cur_step;
                        found_end = true;
                    }
                }
                assert(found_end);
            }
        }
        
        size_t start_position = start_steps.begin()->first;
        size_t end_position = end_step == end_steps.begin()->second ? end_steps.begin()->first : graph.get_position_of_step(end_step);
        bool backward = end_position < start_position;

        return make_pair(backward ? end_position : start_position, backward);
    }
    bool MCMCCaller::is_traversable(const Snarl& snarl) {
        // we need this to be true all the way down to use the RepresentativeTraversalFinder on our snarl.
        bool ret = snarl.start_end_reachable() && snarl.directed_acyclic_net_graph() &&
        graph.has_node(snarl.start().node_id()) && graph.has_node(snarl.end().node_id());
        if (ret == true) {
            const vector<const Snarl*>& children = snarl_manager.children_of(&snarl);
            for (int i = 0; i < children.size() && ret; ++i) {
                ret = is_traversable(*children[i]);
            }
        }
        return ret;
    }
    void MCMCCaller::flatten_common_allele_ends(vcflib::Variant& variant, bool backward) const {
        if (variant.alt.size() == 0) {
            return;
        }
        size_t min_len = variant.alleles[0].length();
        for (int i = 1; i < variant.alleles.size(); ++i) {
            min_len = std::min(min_len, variant.alleles[i].length());
        }
        // want to leave at least one in the reference position
        if (min_len > 0) {
            --min_len;
        }

        bool match = true;
        int shared_prefix_len = 0;
        for (int i = 0; i < min_len && match; ++i) {
            char c1 = std::toupper(variant.alleles[0][!backward ? i : variant.alleles[0].length() - 1 - i]);
            for (int j = 1; j < variant.alleles.size() && match; ++j) {
                char c2 = std::toupper(variant.alleles[j][!backward ? i : variant.alleles[j].length() - 1 - i]);
                match = c1 == c2;
            }
            if (match) {
                ++shared_prefix_len;
            }
        }

        if (!backward) {
            variant.position += shared_prefix_len;
        }
        for (int i = 0; i < variant.alleles.size(); ++i) {
            if (!backward) {
                variant.alleles[i] = variant.alleles[i].substr(shared_prefix_len);
            } else {
                variant.alleles[i] = variant.alleles[i].substr(0, variant.alleles[i].length() - shared_prefix_len);
            }
            if (i == 0) {
                variant.ref = variant.alleles[i];
            } else {
                variant.alt[i - 1] = variant.alleles[i];
            }
        }
    }

}  




