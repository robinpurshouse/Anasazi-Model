#include <stdio.h>
#include <vector>
#include <boost/mpi.hpp>
#include "repast_hpc/AgentId.h"
#include "repast_hpc/RepastProcess.h"
#include "repast_hpc/Utilities.h"
#include "repast_hpc/Properties.h"
#include "repast_hpc/initialize_random.h"
#include "repast_hpc/SVDataSetBuilder.h"
#include "repast_hpc/Point.h"
#include "repast_hpc/Random.h"
#include "repast_hpc/Schedule.h"
#include "repast_hpc/SharedContext.h"
#include "repast_hpc/SharedDiscreteSpace.h"
#include "repast_hpc/GridComponents.h"
#include <string>
#include <fstream>
#include <stdlib.h>
#include "repast_hpc/Moore2DGridQuery.h"

#include "Model.h"

// substracts b<T> to a<T>
template <typename T>
void substract_vector(std::vector<T>& a, const std::vector<T>& b)
{
	typename std::vector<T>::iterator       it = a.begin();
	typename std::vector<T>::const_iterator it2 = b.begin();

	while (it != a.end())
	{
		while (it2 != b.end() && it != a.end())
		{
			if (*it == *it2)
			{
				it = a.erase(it);
				it2 = b.begin();
			}

			else
				++it2;
		}
		if (it != a.end())
			++it;

		it2 = b.begin();
	}
}

AnasaziModel::AnasaziModel(std::string propsFile, int argc, char** argv, boost::mpi::communicator* comm): context(comm) , locationContext(comm)
{
	props = new repast::Properties(propsFile, argc, argv, comm);
	boardSizeX = repast::strToInt(props->getProperty("board.size.x"));
	boardSizeY = repast::strToInt(props->getProperty("board.size.y"));

	initializeRandom(*props, comm);
	repast::Point<double> origin(0,0);
	repast::Point<double> extent(boardSizeX, boardSizeY);
	repast::GridDimensions gd (origin, extent);

	int procX = repast::strToInt(props->getProperty("proc.per.x"));
	int procY = repast::strToInt(props->getProperty("proc.per.y"));
	int bufferSize = repast::strToInt(props->getProperty("grid.buffer"));

	std::vector<int> processDims;
	processDims.push_back(procX);
	processDims.push_back(procY);
	householdSpace = new repast::SharedDiscreteSpace<Household, repast::StrictBorders, repast::SimpleAdder<Household> >("AgentDiscreteSpace",gd,processDims,bufferSize, comm);
	locationSpace = new repast::SharedDiscreteSpace<Location, repast::StrictBorders, repast::SimpleAdder<Location> >("LocationDiscreteSpace",gd,processDims,bufferSize, comm);

	context.addProjection(householdSpace);
	locationContext.addProjection(locationSpace);

	param.startYear = repast::strToInt(props->getProperty("start.year"));
	param.endYear = repast::strToInt(props->getProperty("end.year"));
	param.maxStorageYear = repast::strToInt(props->getProperty("max.store.year"));
	param.maxStorage = repast::strToInt(props->getProperty("max.storage"));
	param.householdNeed = repast::strToInt(props->getProperty("household.need"));
	param.minFissionAge = repast::strToInt(props->getProperty("min.fission.age"));
	param.maxFissionAge = repast::strToInt(props->getProperty("max.fission.age"));
	param.minDeathAge = repast::strToInt(props->getProperty("min.death.age"));
	param.maxDeathAge = repast::strToInt(props->getProperty("max.death.age"));
	param.maxDistance = repast::strToInt(props->getProperty("max.distance"));
	param.initMinCorn = repast::strToInt(props->getProperty("initial.min.corn"));
	param.initMaxCorn = repast::strToInt(props->getProperty("initial.max.corn"));

	param.annualVariance = repast::strToDouble(props->getProperty("annual.variance"));
	param.spatialVariance = repast::strToDouble(props->getProperty("spatial.variance"));
	param.fertilityProbability = repast::strToDouble(props->getProperty("fertility.prop"));
	param.harvestAdjustment = repast::strToDouble(props->getProperty("harvest.adj"));
	param.maizeStorageRatio = repast::strToDouble(props->getProperty("new.household.ini.maize"));

	year = param.startYear;
	stopAt = param.endYear - param.startYear + 1;
	fissionGen = new repast::DoubleUniformGenerator(repast::Random::instance()->createUniDoubleGenerator(0,1));
	deathAgeGen = new repast::IntUniformGenerator(repast::Random::instance()->createUniIntGenerator(param.minDeathAge,param.maxDeathAge));
	yieldGen = new repast::NormalGenerator(repast::Random::instance()->createNormalGenerator(0,param.annualVariance));
	soilGen = new repast::NormalGenerator(repast::Random::instance()->createNormalGenerator(0,param.spatialVariance));
	initAgeGen = new repast::IntUniformGenerator(repast::Random::instance()->createUniIntGenerator(0,param.minDeathAge));
	initMaizeGen = new repast::IntUniformGenerator(repast::Random::instance()->createUniIntGenerator(param.initMinCorn,param.initMaxCorn));

	string resultFile = props->getProperty("result.file");
	out.open(resultFile);
	out << "Year,Number-of-Households" << endl;
}

AnasaziModel::~AnasaziModel()
{
	delete props;
	out.close();
}

void AnasaziModel::initAgents()
{
	int rank = repast::RepastProcess::instance()->rank();

	int LocationID = 0;
	for(int i=0; i<boardSizeX; i++ )
	{
		for(int j=0; j<boardSizeY; j++)
		{
			repast::AgentId id(LocationID, rank, 1);
			Location* agent = new Location(id, soilGen->next());
			locationContext.addAgent(agent);
			locationSpace->moveTo(id, repast::Point<int>(i, j));
			LocationID++;
		}
	}

	readCsvMap();
	readCsvWater();
	readCsvPdsi();
	readCsvHydro();
	int noOfAgents  = repast::strToInt(props->getProperty("count.of.agents"));
	repast::IntUniformGenerator xGen = repast::IntUniformGenerator(repast::Random::instance()->createUniIntGenerator(0,boardSizeX-1));
	repast::IntUniformGenerator yGen = repast::IntUniformGenerator(repast::Random::instance()->createUniIntGenerator(0,boardSizeY-1));
	for(int i=0; i<noOfAgents; i++)
	{
		repast::AgentId id(houseID, rank, 2);
		int initAge = initAgeGen->next();
		int mStorage = initMaizeGen->next();
		Household* agent = new Household(id, initAge, deathAgeGen->next(), mStorage);
		context.addAgent(agent);

		bool houseNotFound = true;
		do {
			int x = xGen.next();
			int y = yGen.next();
			repast::Point<int> locationRepast(x,y);
			Location* randomLocation = locationSpace->getObjectAt(locationRepast);
			if(randomLocation->getState() != 2) {  // as long is not a field (it could be empty or the residence of another household)
				householdSpace->moveTo(id, locationRepast);
				randomLocation->setState(1);
				houseNotFound = false;
			}
		} while(houseNotFound);

		houseID++;
	}

	updateLocationProperties();

	repast::SharedContext<Household>::const_iterator local_agents_iter = context.begin();
	repast::SharedContext<Household>::const_iterator local_agents_end = context.end();

	while(local_agents_iter != local_agents_end)
	{
		Household* household = (&**local_agents_iter);
		if(household->death())
		{
			repast::AgentId id = household->getId();
			local_agents_iter++;

			std::vector<int> houseIntLocation;
			householdSpace->getLocation(id, houseIntLocation);

			std::vector<Location*> locationList;
			if(!houseIntLocation.empty())
			{
				Location* householdLocation = locationSpace->getObjectAt(repast::Point<int>(houseIntLocation));
				householdLocation->setState(0);  // set the household residence to empty
			}
			context.removeAgent(id);
		}
		else
		{
			local_agents_iter++;
			fieldSearch(household);
		}
	}
}

void AnasaziModel::doPerTick()
{
	updateLocationProperties();
	writeOutputToFile();
	year++;
	updateHouseholdProperties();
}

void AnasaziModel::initSchedule(repast::ScheduleRunner& runner)
{
	runner.scheduleEvent(1, 1, repast::Schedule::FunctorPtr(new repast::MethodFunctor<AnasaziModel> (this, &AnasaziModel::doPerTick)));
	runner.scheduleStop(stopAt);
}

void AnasaziModel::readCsvMap()
{
	int x,y,z , mz;
	string zone, maizeZone, temp;

	std::ifstream file ("data/map.csv");//define file object and open map.csv
	file.ignore(500,'\n');//Ignore first line

	while(1)//read until end of file
	{
		getline(file,temp,',');
		if(!temp.empty())
		{
			x = repast::strToInt(temp); //Read until ',' and convert to int & store in x
			getline(file,temp,',');
			y = repast::strToInt(temp); //Read until ',' and convert to int & store in y
			getline(file,temp,','); //colour
			getline(file,zone,',');// read until ',' and store into zone
			getline(file,maizeZone,'\n');// read until next line and store into maizeZone
			if(zone == "\"Empty\"")
			{
				z = 0;
			}
			else if(zone == "\"Natural\"")
			{
				z = 1;
			}
			else if(zone == "\"Kinbiko\"")
			{
				z = 2;
			}
			else if(zone == "\"Uplands\"")
			{
				z = 3;
			}
			else if(zone == "\"North\"")
			{
				z = 4;
			}
			else if(zone == "\"General\"")
			{
				z = 5;
			}
			else if(zone == "\"North Dunes\"")
			{
				z = 6;
			}
			else if(zone == "\"Mid Dunes\"")
			{
				z = 7;
			}
			else if(zone == "\"Mid\"")
			{
				z = 8;
			}
			else
			{
				z = 99;
			}

			if(maizeZone.find("Empty") != std::string::npos)
			{
				mz = 0;
			}
			else if(maizeZone.find("No_Yield") != std::string::npos)
			{
				mz = 1;
			}
			else if(maizeZone.find("Yield_1") != std::string::npos)
			{
				mz = 2;
			}
			else if(maizeZone.find("Yield_2") != std::string::npos)
			{
				mz = 3;
			}
			else if(maizeZone.find("Yield_3") != std::string::npos)
			{
				mz = 4;
			}
			else if(maizeZone.find("Sand_dune") != std::string::npos)
			{
				mz = 5;
			}
			else
			{
				mz = 99;
			}
			std::vector<Location*> locationList;
			locationSpace->getObjectsAt(repast::Point<int>(x, y), locationList);
			locationList[0]->setZones(z,mz);
		}
		else{
			goto endloop;
		}
	}
	endloop: ;
}

void AnasaziModel::readCsvWater()
{
	//read "type","start date","end date","x","y"
	int type, startYear, endYear, x, y;
	string temp;

	std::ifstream file ("data/water.csv");//define file object and open water.csv
	file.ignore(500,'\n');//Ignore first line
	while(1)//read until end of file
	{
		getline(file,temp,',');
		if(!temp.empty())
		{
			getline(file,temp,',');
			getline(file,temp,',');
			getline(file,temp,',');
			type = repast::strToInt(temp); //Read until ',' and convert to int
			getline(file,temp,',');
			startYear = repast::strToInt(temp); //Read until ',' and convert to int
			getline(file,temp,',');
			endYear = repast::strToInt(temp); //Read until ',' and convert to int
			getline(file,temp,',');
			x = repast::strToInt(temp); //Read until ',' and convert to int
			getline(file,temp,'\n');
			y = repast::strToInt(temp); //Read until ',' and convert to int

			std::vector<Location*> locationList;
			locationSpace->getObjectsAt(repast::Point<int>(x, y), locationList);
			locationList[0]->addWaterSource(type,startYear, endYear);
			//locationList[0]->checkWater(existStreams, existAlluvium, x, y, year);
		}
		else
		{
			goto endloop;
		}
	}
	endloop: ;
}

void AnasaziModel::readCsvPdsi()
{
	//read "year","general","north","mid","natural","upland","kinbiko"
	int i=0;
	string temp;

	std::ifstream file ("data/pdsi.csv");//define file object and open pdsi.csv
	file.ignore(500,'\n');//Ignore first line

	while(1)//read until end of file
	{
		getline(file,temp,',');
		if(!temp.empty())
		{
			pdsi[i].year = repast::strToInt(temp); //Read until ',' and convert to int
			getline(file,temp,',');
			pdsi[i].pdsiGeneral = repast::strToDouble(temp); //Read until ',' and convert to double
			getline(file,temp,',');
			pdsi[i].pdsiNorth = repast::strToDouble(temp); //Read until ',' and convert to double
			getline(file,temp,',');
			pdsi[i].pdsiMid = repast::strToDouble(temp); //Read until ',' and convert to double
			getline(file,temp,',');
			pdsi[i].pdsiNatural = repast::strToDouble(temp); //Read until ',' and convert to int
			getline(file,temp,',');
			pdsi[i].pdsiUpland = repast::strToDouble(temp); //Read until ',' and convert to int
			getline(file,temp,'\n');
			pdsi[i].pdsiKinbiko = repast::strToDouble(temp); //Read until ',' and convert to double
			i++;
		}
		else{
			goto endloop;
		}
	}
	endloop: ;
}

void AnasaziModel::readCsvHydro()
{
	//read "year","general","north","mid","natural","upland","kinbiko"
	string temp;
	int i =0;

	std::ifstream file ("data/hydro.csv");//define file object and open hydro.csv
	file.ignore(500,'\n');//Ignore first line

	while(1)//read until end of file
	{
		getline(file,temp,',');
		if(!temp.empty())
		{
			hydro[i].year = repast::strToInt(temp); //Read until ',' and convert to int
			getline(file,temp,',');
			hydro[i].hydroGeneral = repast::strToDouble(temp); //Read until ',' and convert to double
			getline(file,temp,',');
			hydro[i].hydroNorth = repast::strToDouble(temp); //Read until ',' and convert to double
			getline(file,temp,',');
			hydro[i].hydroMid = repast::strToDouble(temp); //Read until ',' and convert to double
			getline(file,temp,',');
			hydro[i].hydroNatural = repast::strToDouble(temp); //Read until ',' and convert to int
			getline(file,temp,',');
			hydro[i].hydroUpland = repast::strToDouble(temp); //Read until ',' and convert to int
			getline(file,temp,'\n');
			hydro[i].hydroKinbiko = repast::strToDouble(temp); //Read until ',' and convert to double
			i++;
		}
		else
		{
			goto endloop;
		}
	}
	endloop: ;
}

int AnasaziModel::yieldFromPdsi(int zone, int maizeZone)
{
	int pdsiValue, row, col;
	switch(zone)
	{
		case 1:
			pdsiValue = pdsi[year-param.startYear].pdsiNatural;
			break;
		case 2:
			pdsiValue = pdsi[year-param.startYear].pdsiKinbiko;
			break;
		case 3:
			pdsiValue = pdsi[year-param.startYear].pdsiUpland;
			break;
		case 4:
		case 6:
			pdsiValue = pdsi[year-param.startYear].pdsiNorth;
			break;
		case 5:
			pdsiValue = pdsi[year-param.startYear].pdsiGeneral;
			break;
		case 7:
		case 8:
			pdsiValue = pdsi[year-param.startYear].pdsiMid;
			break;
		default:
			return 0;
	}

	/* Rows of pdsi table*/
	if(pdsiValue < -3)
	{
		row = 0;
	}
	else if(pdsiValue >= -3 && pdsiValue < -1)
	{
		row = 1;
	}
	else if(pdsiValue >= -1 && pdsiValue < 1)
	{
		row = 2;
	}
	else if(pdsiValue >= 1 && pdsiValue < 3)
	{
		row = 3;
	}
	else if(pdsiValue >= 3)
	{
		row = 4;
	}
	else
	{
		return 0;
	}

	/* Col of pdsi table*/
	if(maizeZone >= 2)
	{
		col = maizeZone - 2;
	}
	else
	{
		return 0;
	}

	return yieldLevels[row][col];
}

double AnasaziModel::hydroLevel(int zone)
{
	switch(zone)
	{
		case 1:
			return hydro[year-param.startYear].hydroNatural;
		case 2:
			return hydro[year-param.startYear].hydroKinbiko;
		case 3:
			return hydro[year-param.startYear].hydroUpland;
		case 4:
		case 6:
			return hydro[year-param.startYear].hydroNorth;
		case 5:
			return hydro[year-param.startYear].hydroGeneral;
		case 7:
		case 8:
			return hydro[year-param.startYear].hydroMid;
		default:
			return 0;
	}
}

void AnasaziModel::checkWaterConditions()
{
	if ((year >= 280 && year < 360) or (year >= 800 && year < 930) or (year >= 1300 && year < 1450))
	{
		existStreams = true;
	}
	else
	{
		existStreams = false;
	}

	if (((year >= 420) && (year < 560)) or ((year >= 630) && (year < 680)) or	((year >= 980) && (year < 1120)) or ((year >= 1180) && (year < 1230)))
	{
		existAlluvium = true;
	}
	else
	{
		existAlluvium = false;
	}
}

void AnasaziModel::writeOutputToFile()
{
	out << year << "," <<  context.size() << std::endl;
}

void  AnasaziModel::updateLocationProperties()
{
	checkWaterConditions();
	int x = 0;
	for(int i=0; i<boardSizeX; i++ )
	{
		for(int j=0; j<boardSizeY; j++)
		{
			std::vector<Location*> locationList;
			locationSpace->getObjectsAt(repast::Point<int>(i, j), locationList);
			locationList[0]->checkWater(existStreams,existAlluvium, i, j, year);
			int mz = locationList[0]->getMaizeZone();
			int z = locationList[0]->getZone();
			int y = yieldFromPdsi(z,mz);
			locationList[0]->calculateYield(y, param.harvestAdjustment, yieldGen->next());
		}
	}
}

void AnasaziModel::updateHouseholdProperties()
{
	int noOfAgents = context.size();
	if (noOfAgents < 1)
		return;
    std::vector<Household*> agents;
    context.selectAgents(repast::SharedContext<Household>::LOCAL, noOfAgents, agents);
    std::vector<Household*>::iterator it = agents.begin();
    while(it != agents.end()) {
        Household* household;
        household = (*it);
        if(household->death())
        {
            removeHousehold(household);
        }
        else
        {
            if(household->fission(param.minFissionAge,param.maxFissionAge, fissionGen->next(), param.fertilityProbability))
            {
                int rank = repast::RepastProcess::instance()->rank();
                repast::AgentId id(houseID, rank, 2);
                int mStorage = household->splitMaizeStored(param.maizeStorageRatio);
                Household* newAgent = new Household(id, 0, deathAgeGen->next(), mStorage);
                context.addAgent(newAgent);

                std::vector<int> loc;
                householdSpace->getLocation(household->getId(), loc);
                householdSpace->moveTo(id, repast::Point<int>(loc[0], loc[1]));
                fieldSearch(newAgent);
                houseID++;
            }

            bool fieldFound = true;
            if(!(household->checkMaize(param.householdNeed)))
            {
                fieldFound = fieldSearch(household);
            }
            if(fieldFound)
            {
                household->nextYear(param.householdNeed);
            }
            
        }
        it++;
    }
}

bool AnasaziModel::fieldSearch(Household* household)
{
	/******** Choose Field ********/
	std::vector<int> houseIntLocation;
	householdSpace->getLocation(household->getId(), houseIntLocation);
	repast::Point<int> center(houseIntLocation);

	std::vector<Location*> neighbouringLocations;
	std::vector<Location*> checkedLocations;
	repast::Moore2DGridQuery<Location> moore2DQuery(locationSpace);

	int maxRangeX = std::max(std::abs(houseIntLocation[0] - boardSizeX), houseIntLocation[0]);
	int maxRangeY = std::max(std::abs(houseIntLocation[1] - boardSizeY), houseIntLocation[1]);
	int maxRange = std::max(maxRangeX, maxRangeY);

	int range = 1;
	while(1)
	{
		moore2DQuery.query(houseIntLocation, range, false, neighbouringLocations);

		for (std::vector<Location*>::iterator it = neighbouringLocations.begin() ; it != neighbouringLocations.end(); ++it)
		{
			Location* tempLoc = (&**it);
			if (std::find(checkedLocations.begin(), checkedLocations.end(), tempLoc) != checkedLocations.end())
			{
				continue;
			}
			checkedLocations.push_back(tempLoc);

			if(tempLoc->getState() == 0)
			{
				if(tempLoc->getExpectedYield() >= param.householdNeed)
				{
					std::vector<int> loc;
					locationSpace->getLocation(tempLoc->getId(), loc);
					household->chooseField(tempLoc);
					goto EndOfLoop;
				}
			}
		}
		neighbouringLocations.clear();
		range++;
		if(range > maxRange)
		{
			removeHousehold(household);
			return false;
		}
	}
	EndOfLoop:
	if(range >= 10)
	{
		return relocateHousehold(household);
	}
	else
	{
		return true;
	}
}

void AnasaziModel::removeHousehold(Household* household)
{
	repast::AgentId id = household->getId();

	std::vector<int> houseIntLocation;
	householdSpace->getLocation(id, houseIntLocation);

	std::vector<Household*> householdList;
	if(!houseIntLocation.empty())
	{
		Location* householdLocation = locationSpace->getObjectAt(repast::Point<int>(houseIntLocation));
		householdSpace->getObjectsAt(repast::Point<int>(houseIntLocation), householdList);
		if(householdList.size() == 1)
		{
			householdLocation->setState(0);  // set the state to empty if there is only one household in this location
		}
		if(household->getAssignedField()!= NULL)
		{
			std::vector<int> fieldIntLocation;
			locationSpace->getLocation(household->getAssignedField()->getId(), fieldIntLocation);
			Location* fieldLocation = locationSpace->getObjectAt(repast::Point<int>(fieldIntLocation));
			fieldLocation->setState(0);
		}
	}

	context.removeAgent(id);
}

bool AnasaziModel::relocateHousehold(Household* household)
{
	std::vector<Location*> fieldNeighbouringLocations;
	std::vector<Location*> suitableHouseholdLocations;
	std::vector<Location*> waterSources;

	std::vector<int> fieldIntLocation, houseIntLocation;
	// get the location of the field and household
	locationSpace->getLocation(household->getAssignedField()->getId(), fieldIntLocation);
	householdSpace->getLocation(household->getId(), houseIntLocation);

	Location* householdLocation = locationSpace->getObjectAt(repast::Point<int>(houseIntLocation));

	int maxRangeX = std::max(std::abs(fieldIntLocation[0] - boardSizeX), fieldIntLocation[0]);
	int maxRangeY = std::max(std::abs(fieldIntLocation[1] - boardSizeY), fieldIntLocation[1]);
	int maxRange = std::max(maxRangeX, maxRangeY);

	repast::Moore2DGridQuery<Location> moore2DQuery(locationSpace);
	int range = floor(param.maxDistance/100);
	int i = 1;
	bool conditionC = true;

	std::vector<Location*> checkedHouseLocations;

	//get all !Field with 1km
	LocationSearch:
		moore2DQuery.query(fieldIntLocation, range*i, false, fieldNeighbouringLocations);
		for (std::vector<Location*>::iterator it = fieldNeighbouringLocations.begin() ; it != fieldNeighbouringLocations.end(); ++it)
		{
			Location* tempHouseLoc = (&**it);
			if (std::find(checkedHouseLocations.begin(), checkedHouseLocations.end(), tempHouseLoc) != checkedHouseLocations.end())
			{
				continue;
			}
			checkedHouseLocations.push_back(tempHouseLoc);

			if(tempHouseLoc->getState() != 2)
			{
				if(householdLocation->getExpectedYield() < tempHouseLoc->getExpectedYield() && conditionC == true)
				{
					suitableHouseholdLocations.push_back(tempHouseLoc);
				}
				if(tempHouseLoc->getWater())
				{
					waterSources.push_back(tempHouseLoc);
				}
			}
		}
		fieldNeighbouringLocations.clear();
		if(suitableHouseholdLocations.size() == 0 || waterSources.size() == 0)
		{
			if(conditionC == true)
			{
				conditionC = false;
			}
			else
			{
				conditionC = true;
				i++;
				if(range*i > maxRange)
				{
					removeHousehold(household);
					return false;
				}
			}
			goto LocationSearch;
		}
		else if(suitableHouseholdLocations.size() == 1)
		{
			std::vector<int> futureHouseIntLocation;
			locationSpace->getLocation(suitableHouseholdLocations[0]->getId(), futureHouseIntLocation);
			householdSpace->moveTo(household->getId(),repast::Point<int>(futureHouseIntLocation));
			return true;
		}
		else
		{
			std::vector<int> point1, point2;
			std::vector<double> minDistances;
			for (std::vector<Location*>::iterator it1 = suitableHouseholdLocations.begin(); it1 != suitableHouseholdLocations.end(); ++it1)
			{
				locationSpace->getLocation((&**it1)->getId(), point1);
				std::vector<double> distances;
				for (std::vector<Location*>::iterator it2 = waterSources.begin(); it2 != waterSources.end(); ++it2)
				{
					locationSpace->getLocation((&**it2)->getId(), point2);
					double distance = sqrt(pow((point1[0]-point2[0]), 2) + pow((point1[1]-point2[1]), 2));
					distances.push_back(distance);
				}
				minDistances.push_back(*std::min_element(distances.begin(), distances.end()));
			}

			// Select the household location with the closest water source
			int minElementIndex = std::min_element(minDistances.begin(), minDistances.end()) - minDistances.begin();
			std::vector<int> futureHouseIntLocation;
			locationSpace->getLocation(suitableHouseholdLocations[minElementIndex]->getId(),futureHouseIntLocation);
			householdSpace->moveTo(household->getId(),repast::Point<int>(futureHouseIntLocation));
			return true;
		}
}
