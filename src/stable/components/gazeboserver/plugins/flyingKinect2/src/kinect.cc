/*
 *  Copyright (C) 2015 JDE Developers Team
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see http://www.gnu.org/licenses/.
 *  Authors :
 *       Victor Arribas Raigadas <v.arribas.urjc@gmai.com>
 */

// OpenCV
#include <opencv2/core/core.hpp>

// Gazebo
#include <gazebo/gazebo.hh>
#include <gazebo/common/common.hh>
#include <gazebo/common/Events.hh>

#include <gazebo/physics/Model.hh>
#include <gazebo/physics/Link.hh>
#include <gazebo/math/Pose.hh>

#include <sensors/DepthCameraSensor.hh>
#include <rendering/DepthCamera.hh>
#include <gazebo/sensors/SensorManager.hh>

// Ice
#include <Ice/Ice.h>
#include <IceUtil/IceUtil.h>
#include <easyiceconfig/EasyIce.h>
#include <easyiceconfig/debug.hpp>

// Jderobot
#include <jderobot/pose3d.h>
#include <quadrotor/interfaces/pushcamerai.h>
#include "src/pointcloudi.hpp"

// debug
#include <quadrotor/debugtools.h>


namespace kinect{

void depth2rgb(const cv::Mat &image_raw, cv::Mat &image_8UC3)
{
	if (image_raw.size() != image_8UC3.size())
		image_8UC3.create(image_raw.rows, image_raw.cols, CV_8UC3);

	float *data = (float*)image_raw.data;
	for ( int i=0; i<image_raw.rows* image_raw.cols; i++ )
	{
		int val = (int)(data[i]*1000);
		image_8UC3.data[3*i+0] = (float)val/10000.*255.;
		image_8UC3.data[3*i+1] = val>>8;
		image_8UC3.data[3*i+2] = val&0xff;
	}
}

#define PARENT_SENSOR_GETS_UPDATES

class KinectPlugin :
        public gazebo::ModelPlugin,
        public jderobot::Pose3D
{
public:
    KinectPlugin(){
        ice_pose3ddata = new jderobot::Pose3DData(0,0,0,0,0,0,0,0);
    }

    virtual ~KinectPlugin(){
        ONDEBUG_INFO(std::cout << _log_prefix << "QuadrotorPlugin::~QuadrotorPlugin()" << std::endl;)
    }

    std::string _log_prefix;

private:


/// Gazebo
protected:
    void Load(gazebo::physics::ModelPtr _model, sdf::ElementPtr _sdf){
        _log_prefix = "["+_model->GetName()+"] ";
        model = _model;

        LoadSensors(_model);
        InitializeIce(_sdf);

        sigintConnection = gazebo::event::Events::ConnectSigInt(
            boost::bind(&KinectPlugin::OnSigInt, this));
    }

    void Init(){
        ONDEBUG_INFO(std::cout << _log_prefix << "KinectPlugin::Init()" << std::endl;)
//        updateConnection = gazebo::event::Events::ConnectWorldUpdateBegin(
//                    boost::bind(&KinectPlugin::OnUpdate, this, _1));

#ifdef PARENT_SENSOR_GETS_UPDATES
        if (camera_sensor){
            camera_sensor->SetActive(true);

            sub_camera = camera_sensor->ConnectUpdated(
                    boost::bind(&KinectPlugin::_on_cam_bootstrap, this));
        }else{
            std::cerr << _log_prefix << "\t camera was not connected (NULL pointer)" << std::endl;
        }
#else
        if(camera_sensor && camera_impl){
            camera_sensor->SetActive(true);

            sub_cam_depth = camera_impl->ConnectNewDepthFrame(
                    boost::bind(&KinectPlugin::_on_cam_update_depth_data, this, _1,_2,_3,_4,_5));
            sub_cam_rgb = camera_impl->ConnectNewImageFrame(
                    boost::bind(&KinectPlugin::_on_cam_update_rgb_data, this, _1,_2,_3,_4,_5));
        }else{
            std::cerr << _log_prefix << "\t camera was not connected (NULL pointer)" << std::endl;
        }
#endif
    }

    void OnUpdate(const gazebo::common::UpdateInfo & /*_info*/){
        _on_pose_update();
    }

    void Reset(){}


    void OnSigInt(){
        if (ic){
            ic->shutdown();
            std::cout<<_log_prefix << "Ice is down now" << std::endl;
        }
    }

    void LoadSensors(gazebo::physics::ModelPtr _model){
        ONDEBUG_INFO(std::cout << _log_prefix << "KinectPlugin::LoadSensors()" << std::endl;)
        gazebo::sensors::SensorManager *sm = gazebo::sensors::SensorManager::Instance();
        unsigned int base_link_id = _model->GetChild(0)->GetId();

        for (gazebo::sensors::SensorPtr s: sm->GetSensors()){
            if (s->GetParentId() == base_link_id){
                camera_sensor = boost::static_pointer_cast<gazebo::sensors::DepthCameraSensor>(s);
                camera_impl = camera_sensor->GetDepthCamera();
            }
        }
    }

private:
    gazebo::physics::ModelPtr model;
    gazebo::event::ConnectionPtr sigintConnection;

/// Gazebo (pose)
    void _on_pose_update(){
        gazebo::math::Pose pose = model->GetWorldPose();
        ice_pose3ddata = new jderobot::Pose3DData(pose.pos.x, pose.pos.y, pose.pos.z, 1, pose.rot.w, pose.rot.x, pose.rot.y, pose.rot.z);
    }

/// Gazebo (cameras)
#ifdef PARENT_SENSOR_GETS_UPDATES
private:
    void _on_cam_bootstrap(){
        camera_sensor->DisconnectUpdated(sub_camera);

        if (img_depth.empty()){
            std::cout <<  _log_prefix << "\tbootstrap depth camera" << std::endl;

            int _width = camera_impl->GetImageWidth();
            int _height = camera_impl->GetImageHeight();

            const float* depth_data = camera_impl->GetDepthData();
            img_depth_raw = cv::Mat(_height, _width, CV_32FC1, (float_t*) depth_data);
            img_depth.create(_height, _width, CV_8UC3);
            depth2rgb(img_depth_raw, img_depth);

            const unsigned char* rgb_data = camera_impl->GetImageData();
            img_rgb = cv::Mat(_height, _width, CV_8UC3, (uint8_t*) rgb_data);

            cam_depthI.onCameraSensorBoostrap(img_depth, nullptr);
            cam_rgbI.onCameraSensorBoostrap(img_rgb, nullptr);
            _on_pose_update();
        }

        sub_camera = camera_sensor->ConnectUpdated(
                    boost::bind(&KinectPlugin::_on_cam_update, this));
    }

    void _on_cam_update(){
        // convert image
        depth2rgb(img_depth_raw, img_depth);

        // stash pose
        _on_pose_update();

        // push data into interfaces
        cam_depthI.onCameraSensorUpdate(img_depth);
        cam_rgbI.onCameraSensorUpdate(img_rgb);
    }
#else
    void _on_cam_bootstrap_depth_data(const float *_data, unsigned int _width, unsigned int _height,
                                      unsigned int, const std::string &){
        camera_impl->DisconnectNewDepthFrame(sub_cam_depth);

        img_depth_raw = cv::Mat(_height, _width, CV_32FC1, (float_t*) _data);
        img_depth.create(_height, _width, CV_8UC3);

        depth2rgb(img_depth_raw, img_depth);
        cam_depthI.onCameraSensorBoostrap(img_depth, nullptr);

        _on_pose_update();

        sub_cam_depth = camera_impl->ConnectNewDepthFrame(
                    boost::bind(&KinectPlugin::_on_cam_update_depth_data, this, _1,_2,_3,_4,_5));
    }

    void _on_cam_update_depth_data(const float *, unsigned int, unsigned int,
                                   unsigned int, const std::string &){
        depth2rgb(img_depth_raw, img_depth);
        cam_depthI.onCameraSensorUpdate(img_depth);

        _on_pose_update();
    }

    void _on_cam_bootstrap_rgb_data(const unsigned char * _data, unsigned int _width, unsigned int _height,
                                      unsigned int, const std::string &){
        camera_impl->DisconnectNewImageFrame(sub_cam_rgb);

        img_rgb = cv::Mat(_height, _width, CV_8UC3, (float_t*) _data);

        cam_rgbI.onCameraSensorBoostrap(img_rgb, nullptr);
        try{
            Ice::Current c;
            jderobot::ImageFormats formats = cam_rgbI.getImageFormat(c);
            std::cout<<"getImageFormat :"<<formats[0]<<std::endl;
        }catch(Ice::Exception &ex){
            std::cout<<"getImageFormat :"<<ex<<std::endl;
        }

        sub_cam_rgb = camera_impl->ConnectNewImageFrame(
                    boost::bind(&KinectPlugin::_on_cam_update_rgb_data, this, _1,_2,_3,_4,_5));
    }

    void _on_cam_update_rgb_data(const unsigned char *, unsigned int, unsigned int,
                                   unsigned int, const std::string &){
        cam_rgbI.onCameraSensorUpdate(img_rgb);
    }

#endif

private:
    gazebo::sensors::DepthCameraSensorPtr camera_sensor;
    gazebo::rendering::DepthCameraPtr camera_impl;

#ifdef PARENT_SENSOR_GETS_UPDATES
    gazebo::event::ConnectionPtr sub_camera;
#else
    gazebo::event::ConnectionPtr sub_cam_depth;
    gazebo::event::ConnectionPtr sub_cam_rgb;
    gazebo::event::ConnectionPtr sub_cam_pcd;
#endif

    cv::Mat img_depth;
    cv::Mat img_depth_raw;
    cv::Mat img_rgb;

/// Ice
    void InitializeIce(sdf::ElementPtr _sdf){
        std::cout << _log_prefix << "KinectPlugin::InitializeIce()" << std::endl;
        std::string iceConfigFile = "flyingKinect2.cfg";
        if(_sdf->HasElement("iceConfigFile"))
            iceConfigFile =  _sdf->GetElement("iceConfigFile")->GetValue()->GetAsString();
        std::cout << _log_prefix << "\tconfig: "<< iceConfigFile << std::endl;

        Ice::InitializationData id;
        id.properties = Ice::createProperties();
        easyiceconfig::loader::loadIceConfig(iceConfigFile, id.properties);
        ic = Ice::initialize(id);

        Ice::ObjectAdapterPtr adapter;
        try{
        adapter = ic->createObjectAdapter("Kinect");

        // pose
        adapter->add(this, ic->stringToIdentity("Pose3d"));
        // pointcloud
        adapter->add(&point_cloudI, ic->stringToIdentity("pointcloud1"));
        // cameras
        adapter->add(&cam_depthI, ic->stringToIdentity("cameraDepth"));
        adapter->add(&cam_rgbI, ic->stringToIdentity("cameraRGB"));

        adapter->activate();
        }catch(Ice::Exception &ex){ std::cout<< ex << std::endl;}

        std::cout<<_log_prefix << "Ice adapter listening at " << std::endl;
        std::cout<<_log_prefix << "\t" << adapter->getEndpoints()[0]->toString() << std::endl;
    }

protected:
    Ice::CommunicatorPtr ic;


/// Ice (Pose3D)
private:
    jderobot::Pose3DDataPtr ice_pose3ddata;
public:
    jderobot::Pose3DDataPtr
    getPose3DData ( const Ice::Current& ){
        return ice_pose3ddata;
    }

    Ice::Int
    setPose3DData ( const jderobot::Pose3DDataPtr & pose3dData,
                                     const Ice::Current& ){
        gazebo::math::Pose pose(
                    gazebo::math::Vector3(pose3dData->x, pose3dData->y, pose3dData->z),
                    gazebo::math::Quaternion(pose3dData->q0, pose3dData->q1, pose3dData->q2, pose3dData->q3)
        );

        model->SetWorldPose(pose);
        return 0;
    }

/// Ice (PCL
private:
    jderobot::interfaces::PointCloudI point_cloudI;

/// Ice (Camera)
private:
    quadrotor::interfaces::PushCameraI cam_depthI;
    quadrotor::interfaces::PushCameraI cam_rgbI;

};

GZ_REGISTER_MODEL_PLUGIN(kinect::KinectPlugin)

}//NS
