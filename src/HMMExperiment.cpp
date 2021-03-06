//
//  HMMExperiment.cpp
//  bdg_hmm
//
//  Created by Bruno Di Giorgi on 14/06/16.
//
//

#include "HMMExperiment.hpp"
#include <iostream>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <string.h>
#include <algorithm>
#include "Core.hpp"

namespace bdg {
    
    HMMExperiment::HMMExperiment(std::string dataset, std::string logfile_, std::string logprogressfile_,
                                 int hs_, int nfolds_, bool shuffle_, int EMiterations_, int nworkers_,
                                 PredictionType prediction_type_, TestingStrategy testing_strat_)
    : logfile(logfile_, std::ofstream::app), logprogressfile(logprogressfile_, std::ofstream::out) {
        assert(logfile.good());
        assert(logprogressfile.good());
        
        nworkers = nworkers_;
        for(int iworkers=0; iworkers<nworkers; iworkers++)
            workers.push_back(HMMWorkerThread());
        
        hs = hs_;
        nfolds = nfolds_;
        shuffle = shuffle_;
        EMiterations = EMiterations_;
        prediction_type = prediction_type_;
        testing_strat = testing_strat_;
        
        set_dataset(dataset);
    }
    
    HMMExperiment::~HMMExperiment() {
        deallocate_aux();
        
        logfile.close();
        
        if(folds != nullptr) {delete folds; folds = nullptr;}
        if(hmm != nullptr) {delete hmm; hmm = nullptr;}
        if(obs != nullptr) {delete obs; obs = nullptr;}
        if(d != nullptr) {delete d; d = nullptr;}
    }
    
    void HMMExperiment::allocate_aux() {
        N = new double[hs*hs];
        Naux1 = new double[hs*hs];
        M = new double[hs*os];
        Maux = new double[hs*os];
        NU = new double[hs];
        
        EM_loglik = new double[EMiterations];
        
        int length = EMiterations;
        if(testing_strat == TestingStrategy::test_last)
            length = 1;
        if(testing_strat == TestingStrategy::test_odd)
            length = EMiterations / 2;
        EM_te_entropy = new double[length];
        EM_tr_entropy = new double[length];
        EM_test_it = new double[length];
        
        tr_set = new int[obs->nseq];
        te_set = new int[obs->nseq];
    }
    
    void HMMExperiment::deallocate_aux() {
        
        if(tr_set != nullptr) {delete[] tr_set; tr_set = nullptr;}
        if(te_set != nullptr) {delete[] te_set; te_set = nullptr;}
        
        if(NU != nullptr) {delete[] NU; NU = nullptr;}
        if(Maux != nullptr){delete[] Maux; Maux = nullptr;}
        if(M != nullptr) {delete[] M; M = nullptr;}
        if(Naux1 != nullptr) {delete[] Naux1; Naux1 = nullptr;}
        if(N != nullptr) {delete[] N; N = nullptr;}
        
        if(EM_loglik != nullptr) {delete[] EM_loglik; EM_loglik = nullptr;}
        if(EM_te_entropy != nullptr) {delete[] EM_te_entropy; EM_te_entropy = nullptr;}
        if(EM_tr_entropy != nullptr) {delete[] EM_tr_entropy; EM_tr_entropy = nullptr;}
        if(EM_test_it != nullptr) {delete[] EM_test_it; EM_test_it = nullptr;}
    }
    
    void HMMExperiment::set_dataset(std::string dataset) {
        if(d != nullptr) {delete d; d = nullptr;}
        if(obs != nullptr) {delete obs; obs = nullptr;}
        if(folds != nullptr) {delete folds; folds = nullptr;}
        if(hmm != nullptr) {delete hmm; hmm = nullptr;}
        
        d = new Dataset(dataset);
        obs = new HMMObservations(*d);
        os = d->alphabet_size;
        folds = new Folds(d->nseq, nfolds, shuffle);
        hmm = new HMM(hs, os);
        hmm->set_observations(obs);
        
        for(HMMWorkerThread & w : workers)
            w.set_hmm(hmm);
        
        deallocate_aux();
        allocate_aux();
    }
    
    void HMMExperiment::set_nseq(int nseq) {
        if(folds != nullptr) {delete folds; folds = nullptr;}
        folds = new Folds(nseq, nfolds, shuffle);
    }
    
    void HMMExperiment::set_hs(int hs_) {
        hs = hs_;
        
        logprogressfile << "*** hidden size: " << hs << " *** " << std::endl;
        
        if(hmm != nullptr) {delete hmm; hmm = nullptr;}
        hmm = new HMM(hs, os);
        hmm->set_observations(obs);
        
        for(HMMWorkerThread & w : workers)
            w.set_hmm(hmm);
        
        deallocate_aux();
        allocate_aux();
    }
    
    void HMMExperiment::load_parameters(std::string filename) {
        hmm->load_parameters(filename);
    }
    
    void HMMExperiment::run() {
        
        for (int ifold = 0; ifold < nfolds; ifold++) {
            folds->get_fold(ifold, tr_set, &tr_size, te_set, &te_size);
            logprogressfile << "ifold: " << ifold << std::endl;
            
            auto start_t = std::time(nullptr);
            auto start_time = *std::localtime(&start_t);
            char start_time_str [80];
            strftime(start_time_str, 80, "%F %X", &start_time);
            
            hmm->init_random_parameters();
            
            // subdivide sequences into worker threads
            assign_sequences_to_workers(tr_size, tr_set, te_size, te_set);
            
            // learn parameters
            double absdif;
            EM_test_counter = 0;
            for(int it = 0; it < EMiterations; it++) {
                
                for(HMMWorkerThread & w : workers)
                    w.run_Estep();
                for(HMMWorkerThread & w : workers)
                    w.join();
                
                EM_loglik[it] = 0;
                memset(NU, 0, hs*sizeof(double));
                memset(N, 0, hs*hs*sizeof(double));
                memset(M, 0, hs*os*sizeof(double));
                for(HMMWorkerThread & w : workers) {
                    EM_loglik[it] += w.loglik;
                    vadd(NU, 1, w.NU, 1, NU, 1, hs);
                    vadd(N, 1, w.N, 1, N, 1, hs*hs);
                    vadd(M, 1, w.M, 1, M, 1, hs*os);
                }
                
                hmm->Mstep(N, M, NU, Naux1, Maux, &absdif);
                
                auto now = std::time(nullptr);
                auto now_ = *std::localtime(&now);
                char now_str [80];
                strftime(now_str, 80, "%F %X", &now_);
                logprogressfile << now_str << " ";
                
                logprogressfile << "EM iteration " << it << ", loglik = " << EM_loglik[it] << std::endl;
                
                if((testing_strat == TestingStrategy::test_every) ||
                   ((testing_strat == TestingStrategy::test_odd) && (it % 2 == 1)) ||
                   ((testing_strat == TestingStrategy::test_last) && (it == EMiterations-1))) {
                    
                    if(prediction_type == PredictionType::viterbi){
                        for(HMMWorkerThread & w : workers)
                            w.run_crossentropy_viterbi();
                    } else if(prediction_type == PredictionType::posterior) {
                        for(HMMWorkerThread & w : workers)
                            w.run_crossentropy_posterior();
                    }
                    
                    for(HMMWorkerThread & w : workers)
                        w.join();
                    
                    double tr_entropy = 0;
                    double te_entropy = 0;
                    
                    int tr_count = 0;
                    int te_count = 0;
                    for(HMMWorkerThread & w : workers) {
                        tr_entropy += w.tr_entropy;
                        tr_count += w.tr_count;
                        te_entropy += w.te_entropy;
                        te_count += w.te_count;
                    }
                    
                    EM_tr_entropy[EM_test_counter] = tr_entropy / tr_count;
                    EM_te_entropy[EM_test_counter] = te_entropy / te_count;
                    EM_test_it[EM_test_counter] = it;
                    
                    now = std::time(nullptr);
                    now_ = *std::localtime(&now);
                    strftime(now_str, 80, "%F %X", &now_);
                    logprogressfile << now_str << " ";
                    
                    logprogressfile << "tr_entropy: " << EM_tr_entropy[EM_test_counter] <<
                    ", te_entropy: " << EM_te_entropy[EM_test_counter] << std::endl;
                    EM_test_counter++;
                    
                }
            }
            
            // hmm->print_parameters();
            std::string filename = model_filename();
            hmm->save_parameters(filename);
            
            // LOG
            auto end_t = std::time(nullptr);
            auto end_time = *std::localtime(&end_t);
            char end_time_str [80];
            strftime(end_time_str, 80, "%F %X", &end_time);
            
            auto duration = std::difftime(end_t, start_t);
            logfile << "{" << std::endl;
            logfile << "\"start_time\": \"" << start_time_str << "\", " << std::endl;
            logfile << "\"end_time\": \"" << end_time_str << "\", " << std::endl;
            logfile << "\"duration\": " << duration << ", " << std::endl;
            logfile << "\"Dataset\": \"" << d->filename << "\", " << std::endl;
            logfile << "\"ifold\": " << ifold << ", " << std::endl;
            logfile << "\"nfold\": " << folds->nfolds << ", " << std::endl;
            logfile << "\"nseq\": " << folds->n << ", " << std::endl;
            logfile << "\"EMiterations\": " << EMiterations << ", " << std::endl;
            logfile << "\"EM_loglik\": " << "[" << v_to_str(EMiterations, EM_loglik) << "], " << std::endl;
            logfile << "\"EM_te_entropy\": " << "[" << v_to_str(EM_test_counter, EM_te_entropy) << "], " << std::endl;
            logfile << "\"EM_tr_entropy\": " << "[" << v_to_str(EM_test_counter, EM_tr_entropy) << "], " << std::endl;
            logfile << "\"EM_test_it\": " << "[" << v_to_str(EM_test_counter, EM_test_it) << "], " << std::endl;
            logfile << "\"hs\": " << hs << ", " << std::endl;
            logfile << "\"nworkers\": " << nworkers << ", " << std::endl;
            logfile << "\"entropy\": " << EM_te_entropy[EM_test_counter - 1] << ", " << std::endl;
            logfile << "\"fn_params\": \"" << filename << "\", " << std::endl;
            logfile << "}," << std::endl;
            logfile << std::endl;
            
        }  // for ifold
    }
    
    void HMMExperiment::assign_sequences_to_workers(int tr_size, int* tr_set, int te_size, int* te_set) {
        double tr_k = tr_size / (float)nworkers;
        double te_k = te_size / (float)nworkers;
        int tr_imin = 0;
        int te_imin = 0;
        for (int i=0; i<nworkers; i++) {
            int tr_imax = floor((i+1) * tr_k);  // not included
            if(tr_imax == tr_imin)
                tr_imax = std::min({tr_imax+1, tr_size});
            int tr_n = std::max({tr_imax - tr_imin, 0});
            
            int te_imax = floor((i+1) * te_k);  // not included
            if(te_imax == te_imin)
                te_imax = std::min({te_imax+1, te_size});
            int te_n = std::max({te_imax - te_imin, 0});
            workers[i].set_seqs(tr_n, tr_set + tr_imin, te_n, te_set + te_imin);
            
            tr_imin = std::max({tr_imin, tr_imax});
            te_imin = std::max({te_imin, te_imax});
        }
    }
    
    std::string HMMExperiment::model_filename() {
        std::ostringstream filename;
        filename << "models/" << time(0) << ".model";
        return filename.str();
    }
    
    
}