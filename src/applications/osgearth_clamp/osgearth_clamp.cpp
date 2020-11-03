/* -*-c++-*- */
/* osgEarth - Geospatial SDK for OpenSceneGraph
* Copyright 2020 Pelican Mapping
* http://osgearth.org
*
* osgEarth is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/

#include <osgEarth/Notify>
#include <osgEarth/MapNode>
#include <osgEarth/OGRFeatureSource>
#include <osgEarth/Feature>
#include <osgEarth/TerrainTileModelFactory>
#include <OpenThreads/Thread>

#define LC "[clamp] "

using namespace osgEarth;
using namespace osgEarth::Util;

int
usage(const char* name, const std::string& error)
{
    OE_NOTICE 
        << "Clamps shapefile features to terrain and writes out a new shapefile."
        << "\nError: " << error
        << "\nUsage:"
        << "\n" << name
        << "\n  <earthfile>          ; earth file containing elevation layer"
        << "\n  --in in.shp          ; input features to clamp"
        << "\n  --out out.shp        ; output features"
        << "\n  --attribute <name>   ; attribute in which to store elevation value"
        << "\n  [--quiet]            ; suppress console output"
        << std::endl;

    return -1;
}

struct App
{
    osg::ref_ptr<MapNode> mapNode;
    const Map* map;
    osg::ref_ptr<OGRFeatureSource> input;
    osg::ref_ptr<OGRFeatureSource> output;
    Threading::Mutexed<std::queue<FeatureList*> > outputQueue;
    Threading::Event gate;
    std::string attrName;
    bool verbose;

    App() : verbose(true) { }

    int open(int argc, char** argv)
    {
        osg::ArgumentParser arguments(&argc, argv);

        verbose = !arguments.read("--quiet");

        std::string infile;
        if (!arguments.read("--in", infile))
            return usage(argv[0], "Missing --in");

        std::string outfile;
        if (!arguments.read("--out", outfile))
            return usage(argv[0], "Missing --out");

        if (!arguments.read("--attribute", attrName))
            return usage(argv[0], "Missing --attribute");

        mapNode = MapNode::load(arguments);
        if (!mapNode.valid())
            return usage(argv[0], "No earth file");

        // open input shapefile
        input = new OGRFeatureSource();
        input->setURL(infile);
        if (input->open().isError())
            return usage(argv[0], input->getStatus().message());

        // create output shapefile
        FeatureSchema outSchema;
        outSchema = input->getSchema();
        outSchema[attrName] = ATTRTYPE_DOUBLE;
        output = new OGRFeatureSource();
        output->setOGRDriver("ESRI Shapefile");
        output->setURL(outfile);
        if (output->create(input->getFeatureProfile(), outSchema, input->getGeometryType(), NULL).isError())
            return usage(argv[0], output->getStatus().toString());

        return 0;
    }

    void run()
    {
        ElevationPool::WorkingSet workingSet;

        unsigned total = input->getFeatureCount();
        unsigned count = 0u;

        std::cout << "\n";

        GeoPoint point(input->getFeatureProfile()->getSRS(),0,0,0);

        osg::ref_ptr<FeatureCursor> cursor = input->createFeatureCursor(Query(), NULL);
        while(cursor->hasMore())
        {
            Feature* f = cursor->nextFeature();
            GeoExtent e = f->getExtent();
            point.vec3d() = e.getCentroid();

            ElevationSample sample = map->getElevationPool()->getSample(
                point,
                &workingSet);
            
            float value = sample.elevation().as(Units::METERS);
            if (value == NO_DATA_VALUE)
                value = 0.0f;

            f->set(attrName, value);

            output->insertFeature(f);

            if (verbose)
            {
                ++count;
                if (count==1 || count%1000==0 || count==total)
                    std::cout << "\r" << count << "/" << total << std::flush;
            }
        }

        if (verbose)
            std::cout << std::endl;
    }
};

int
main(int argc, char** argv)
{
    App app;

    if (app.open(argc, argv) < 0)
        return -1;

    app.run();

    if (app.verbose)
        std::cout << "\nBuilding index..." << std::flush;

    app.output->buildSpatialIndex();
    app.output->close();

    if (app.verbose)
        std::cout << "\rDone!            " << std::endl;
}