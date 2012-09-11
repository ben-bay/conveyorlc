/* 
 * File:   preLigands.cpp
 * Author: zhang30
 *
 * Created on August 13, 2012, 4:08 PM
 */

#include <cstdlib>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>

#include "src/Parser/Sdf.h"
#include "src/Parser/Pdb.h"
#include "src/MM/Amber.h"
#include "src/Parser/SanderOutput.h"
#include "src/Structure/Sstrm.hpp"
#include "src/Structure/Constants.h"
#include "src/Common/File.hpp"
#include "src/Common/Tokenize.hpp"
#include "src/Common/LBindException.h"
#include "src/XML/XMLHeader.hpp"

#include <boost/scoped_ptr.hpp>

#include <mpi.h>

using namespace LBIND;


/*!
 * \breif preReceptors calculation receptor grid dimension from CSA sitemap output
 * \param argc
 * \param argv argv[1] takes the input file name
 * \return success 
 * \defgroup preReceptors_Commands preReceptors Commands
 *
 * 
 * Usage on HPC slurm
 * 
 * \verbatim
 
    export AMBERHOME=/usr/gapps/medchem/amber/amber12
    export PATH=$AMBERHOME/bin/:$PATH
    export LIGDIR=`pwd`

    srun -N4 -n48 -ppdebug /g/g92/zhang30/medchem/NetBeansProjects/MedCM/apps/mmpbsa/preLignds  <input-file.sdf>

    <input-file.sdf>: multi-structure SDF file contains all ligands information.

    Requires: define LIGDIR 
   \endverbatim
 */


bool preLigands(std::string& dir) {
    
    bool jobStatus=true;
    // ! Goto sub directory
    std::string LIGDIR=getenv("LIGDIR");
    std::string subDir=LIGDIR+"/"+dir;   
    chdir(subDir.c_str());

    std::string sdfFile="ligand.sdf";
    std::string pdb1File="ligand.pdb";
    
    std::string cmd="babel -isdf " + sdfFile + " -opdb " +pdb1File;
    std::cout << cmd << std::endl;
    system(cmd.c_str());
    
    std::string pdbFile="ligrn.pdb";
    std::string tmpFile="ligstrp.pdb";
    
    boost::scoped_ptr<Pdb> pPdb(new Pdb());   
    
    //! Rename the atom name.
    pPdb->renameAtom(pdb1File, pdbFile);
    
    pPdb->strip(pdbFile, tmpFile);
    
    //! Get ligand charge from SDF file.
    std::string keyword="PUBCHEM_TOTAL_CHARGE";
    
    boost::scoped_ptr<Sdf> pSdf(new Sdf());
    std::string info=pSdf->getInfo(sdfFile, keyword);
        
    std::cout << "Charge:" << info << std::endl;
    int charge=Sstrm<int, std::string>(info);
    std::string chargeStr=Sstrm<std::string,int>(charge);
    
    std::stringstream ss;
    
    ss << charge;

    //! Start antechamber calculation
    std::string output="ligand.mol2";
    std::string options=" -c bcc -nc "+ chargeStr;
        
    boost::scoped_ptr<Amber> pAmber(new Amber());
    pAmber->antechamber(tmpFile, output, options);
    
    pAmber->parmchk(output);
    
    //! leap to obtain forcefield for ligand
    std::string ligName="LIG";
    std::string tleapFile="leap.in";
    
    pAmber->tleapInput(output,ligName,tleapFile);
    pAmber->tleap(tleapFile); 
    
    std::string checkFName="LIG.prmtop";
    if(!fileExist(checkFName)){
        std::string message="LIG.prmtop does not exist.";
        throw LBindException(message); 
        jobStatus=false; 
        return jobStatus;        
    }
   
    //! GB energy minimization
    std::string minFName="LIG_minGB.in";
    {
        std::ofstream minFile;
        try {
            minFile.open(minFName.c_str());
        }
        catch(...){
            std::string mesg="mmpbsa::receptor()\n\t Cannot open min file: "+minFName;
            throw LBindException(mesg);
        }   

        minFile << "title..\n" 
                << "&cntrl\n" 
                << "  imin   = 1,\n" 
                << "  ntmin   = 3,\n" 
                << "  maxcyc = 2000,\n" 
                << "  ncyc   = 1000,\n" 
                << "  ntpr   = 200,\n" 
                << "  ntb    = 0,\n" 
                << "  igb    = 5,\n" 
                << "  cut    = 15,\n"       
                << " /\n" << std::endl;

        minFile.close();    
    }          
    
    cmd="sander  -O -i LIG_minGB.in -o LIG_minGB.out  -p LIG.prmtop -c LIG.inpcrd -ref LIG.inpcrd  -x LIG.mdcrd -r LIG_min.rst";
    std::cout <<cmd <<std::endl;
    system(cmd.c_str()); 
    boost::scoped_ptr<SanderOutput> pSanderOutput(new SanderOutput());
    std::string sanderOut="LIG_minGB.out";
    double ligGBen=pSanderOutput->getEnergy(sanderOut);
    
    if(abs(ligGBen)<NEARZERO){
        std::string message="Ligand GB minimization fails.";
        throw LBindException(message); 
        jobStatus=false; 
        return jobStatus;          
    }
    
    //! Use ambpdb generated PDB file for PDBQT.
    cmd="ambpdb -p LIG.prmtop < LIG_min.rst > LIG_min.pdb";
    std::cout <<cmd <<std::endl;
    system(cmd.c_str());    

    checkFName="LIG_min.pdb";
    if(!fileExist(checkFName)){
        std::string message="LIG_min.pdb minimization PDB file does not exist.";
        throw LBindException(message); 
        jobStatus=false; 
        return jobStatus;        
    }
    
    
    //! Get DPBQT file for ligand from minimized structure.
    cmd="prepare_ligand4.py -l LIG_min.pdb -A hydrogens";
    std::cout << cmd << std::endl;
    system(cmd.c_str());
    
    checkFName="LIG_min.pdbqt";
    if(!fileExist(checkFName)){
        std::string message="LIG_min.pdbqt PDBQT file does not exist.";
        throw LBindException(message); 
        jobStatus=false; 
        return jobStatus;        
    } 
    
    //! PB energy minimization
    minFName="LIG_minPB.in";
    {
        std::ofstream minFile;
        try {
            minFile.open(minFName.c_str());
        }
        catch(...){
            std::string mesg="mmpbsa::receptor()\n\t Cannot open min file: "+minFName;
            throw LBindException(mesg);
        }   

        minFile << "title..\n" 
                << " &cntrl\n" 
                << "  imin   = 1,\n" 
                << "  ntmin   = 3,\n" 
                << "  maxcyc = 2000,\n" 
                << "  ncyc   = 1000,\n" 
                << "  ntpr   = 200,\n" 
                << "  ntb    = 0,\n" 
                << "  igb    = 10,\n" 
                << "  cut    = 15,\n"         
                << " /\n"
                << " &pb\n"
                << "  npbverb=0, epsout=80.0, radiopt=1, space=0.5,\n"
                << "  accept=1e-4, fillratio=6, sprob=1.6\n"
                << " / \n" << std::endl;

        minFile.close();    
    }          
    
    cmd="sander  -O -i LIG_minPB.in -o LIG_minPB.out  -p LIG.prmtop -c LIG_min.rst -ref LIG_min.rst  -x LIG.mdcrd -r LIG_min2.rst";
    std::cout <<cmd <<std::endl;
    system(cmd.c_str());       

    sanderOut="LIG_minPB.out";
    double ligPBen=pSanderOutput->getEnergy(sanderOut);
    
    if(abs(ligPBen)<NEARZERO){
        std::string message="Ligand PB minimization fails.";
        throw LBindException(message); 
        jobStatus=false; 
        return jobStatus;          
    }    
    return jobStatus;
}


//void saveStrList(std::string& fileName, std::vector<std::string>& strList){
//    std::ifstream inFile;
//    try {
//        inFile.open(fileName.c_str());
//    }
//    catch(...){
//        std::cout << "Cannot open file: " << fileName << std::endl;
//    } 
//
//    std::string fileLine;
//    while(inFile){
//        std::getline(inFile, fileLine);
//        std::vector<std::string> tokens;
//        tokenize(fileLine, tokens); 
//        if(tokens.size() > 0){
//            strList.push_back(tokens[0]);
//        }        
//    }
//    
//}

struct JobInputData{        
    char dirBuffer[100];
};

struct JobOutData{  
    bool error;
    char dirBuffer[100];
    char message[100];
};

int main(int argc, char** argv) {
    
    int nproc, rank, rc;

    int jobFlag=1; // 1: doing job,  0: done job
    
    JobInputData jobInput;
    JobOutData jobOut;
    
    MPI_Status status1, status2;
        
    int rankTag=1;
    int jobTag=2;

    int inpTag=3;
    int outTag=4;

    rc = MPI_Init(&argc, &argv);
    if (rc != MPI_SUCCESS) {
        std::cerr << "Error starting MPI program. Terminating.\n";
        MPI_Abort(MPI_COMM_WORLD, rc);
    }

    MPI_Comm_size(MPI_COMM_WORLD, &nproc);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    
//    MPI_Barrier(MPI_COMM_WORLD);
    double time=MPI_Wtime();

    if (nproc < 2) {
        std::cerr << "Error: Total process less than 2" << std::endl;
        return 1;
    }
    
    std::string inputFName=argv[1];
    
    if(inputFName.size()==0){
        std::cerr << "Usage: preLigands <input.sdf>" << std::endl;
        return 1;        
    }

    std::cout << "Number of tasks= " << nproc << " My rank= " << rank << std::endl;

    if (rank == 0) {
        
        std::string LIGDIR=getenv("LIGDIR");
        chdir(LIGDIR.c_str());
        int count=1;

        std::ifstream inFile;
        try {
            inFile.open(inputFName.c_str());
        }
        catch(...){
            std::cout << "preLigands >> Cannot open file" << inputFName << std::endl;
        }
        
//! Tracking error using XML file

	XMLDocument doc;  
 	XMLDeclaration* decl = new XMLDeclaration( "1.0", "", "" );  
	doc.LinkEndChild( decl );  
 
	XMLElement * root = new XMLElement( "Errors" );  
	doc.LinkEndChild( root );  

	XMLComment * comment = new XMLComment();
	comment->SetValue(" Tracking calculation error using XML file " );  
	root->LinkEndChild( comment );  
         
        FILE* xmlFile=fopen("JobTrackingTemp.xml", "w"); 
 //! END of XML header
        
        const std::string delimter="$$$$";
        std::string fileLine="";
        std::string contents="";

        while(inFile){
            std::getline(inFile, fileLine);
            contents=contents+fileLine+"\n";
            if(fileLine.size()>=4 && fileLine.compare(0,4, delimter)==0){
                std::string dir=Sstrm<std::string, int>(count);

                std::string cmd="mkdir -p " +dir;
                std::cout << cmd << std::endl;
                system(cmd.c_str());
                
                std::string outputFName=dir+"/ligand.sdf";
                std::ofstream outFile;
                try {
                    outFile.open(outputFName.c_str());
                }
                catch(...){
                    std::cout << "preLigands >> Cannot open file" << outputFName << std::endl;
                }
                
                outFile <<contents;
                outFile.close(); 
                
                contents=""; //! clean up the contents for the next structure.
                
                
                if(count >nproc-1){
                    MPI_Recv(&jobOut, sizeof(JobOutData), MPI_CHAR, MPI_ANY_SOURCE, outTag, MPI_COMM_WORLD, &status2);
//                    if(!jobOut.error){
                        XMLElement * element = new XMLElement("error"); 
                        element->SetAttribute("ligand", jobOut.dirBuffer);
                        element->SetAttribute("mesg", jobOut.message);
                        element->Print(xmlFile,2);
                        fputs("\n",xmlFile);
                        fflush(xmlFile);
                        root->LinkEndChild(element);    
//                    }
                } 
                
                int freeProc;
                MPI_Recv(&freeProc, 1, MPI_INTEGER, MPI_ANY_SOURCE, rankTag, MPI_COMM_WORLD, &status1);
                std::cout << "At Process: " << freeProc << " working on: " << dir << std::endl;
                MPI_Send(&jobFlag, 1, MPI_INTEGER, freeProc, jobTag, MPI_COMM_WORLD); 

                strcpy(jobInput.dirBuffer, dir.c_str());

                MPI_Send(&jobInput, sizeof(JobInputData), MPI_CHAR, freeProc, inpTag, MPI_COMM_WORLD);   
               
                ++count;               
            }          
        }        
//        for(unsigned i=0; i<dirList.size(); ++i){
//            std::cout << "Working on " << dirList[i] << std::endl;
//            int freeProc;
//            MPI_Recv(&freeProc, 1, MPI_INTEGER, MPI_ANY_SOURCE, rankTag, MPI_COMM_WORLD, &status1);
//            MPI_Send(&jobFlag, 1, MPI_INTEGER, freeProc, jobTag, MPI_COMM_WORLD); 
//
//            strcpy(jobInput.dirBuffer, dirList[i].c_str());
//
//            MPI_Send(&jobInput, sizeof(JobInputData), MPI_CHAR, freeProc, inpTag, MPI_COMM_WORLD);
//
//        }
        int nJobs=count-1;
        int ndata=(nJobs<nproc-1)? nJobs: nproc-1;
        std::cout << "ndata=" << ndata << " nJobs=" << nJobs << std::endl;
    
        for(unsigned i=0; i < ndata; ++i){
            MPI_Recv(&jobOut, sizeof(JobOutData), MPI_CHAR, MPI_ANY_SOURCE, outTag, MPI_COMM_WORLD, &status2);
//            if(!jobOut.error){
                XMLElement * element = new XMLElement("error"); 
                element->SetAttribute("ligand", jobOut.dirBuffer);
                element->SetAttribute("mesg", jobOut.message);
                element->Print(xmlFile,2);
                fputs("\n",xmlFile);
                fflush(xmlFile);
                root->LinkEndChild(element);                  
//            }
        } 
        
        doc.SaveFile( "JobTracking.xml" );
        
        for(unsigned i=1; i < nproc; ++i){
            int freeProc;
            MPI_Recv(&freeProc, 1, MPI_INTEGER, MPI_ANY_SOURCE, rankTag, MPI_COMM_WORLD, &status1);
            jobFlag=0;;
            MPI_Send(&jobFlag, 1, MPI_INTEGER, freeProc, jobTag, MPI_COMM_WORLD);            
        }
        
    }else {
        while (1) {
            MPI_Send(&rank, 1, MPI_INTEGER, 0, rankTag, MPI_COMM_WORLD);
            MPI_Recv(&jobFlag, 20, MPI_CHAR, 0, jobTag, MPI_COMM_WORLD, &status2);
            if (jobFlag==0) {
                break;
            }
            // Receive parameters

            MPI_Recv(&jobInput, sizeof(JobInputData), MPI_CHAR, 0, inpTag, MPI_COMM_WORLD, &status1);
                        
            std::string dir=jobInput.dirBuffer;
            strcpy(jobOut.message, "Finished!");
            try{
                bool jobStatus=preLigands(dir);            
                jobOut.error=jobStatus;
            } catch (LBindException& e){
                std::string message= e.what();  
                strcpy(jobOut.message, message.c_str());
            }
            strcpy(jobOut.dirBuffer, dir.c_str());
            MPI_Send(&jobOut, sizeof(JobOutData), MPI_CHAR, 0, outTag, MPI_COMM_WORLD);
        }
    }


    time=MPI_Wtime()-time;
    std::cout << "Rank= " << rank << " MPI Wall Time= " << time << std::endl;
    MPI_Finalize();
    
    
    return 0;
}
