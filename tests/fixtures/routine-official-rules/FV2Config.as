package mmo.activityext.newact20260522.farmv2.model
{
   import mmo.common.DateUtil;
   import mmo.play.compatible.H5;
   
   public class FV2Config
   {
      
      private static const PRE_URL:String = H5.s("activityext/20260522/farmv2/","newactivityext/newact20260522/farmv2/");
      
      private static const PRE_CLS:String = "mmo.activityext.newact20260522.farmv2.";
      
      private static const ActStartDate:Number = DateUtil.parse("20260731",true).getTime();
      
      public static const FARM_DEFINE:Array = [{
         "level":1,
         "name":"普通农场",
         "refreshRate":0.315,
         "chestRate":0,
         "color":"褐",
         "guarantee":[]
      },{
         "level":2,
         "name":"青翠农场",
         "refreshRate":0.3,
         "chestRate":0,
         "color":"绿",
         "guarantee":[{
            "rarity":1,
            "count":5
         }]
      },{
         "level":3,
         "name":"肥沃农场",
         "refreshRate":0.3,
         "chestRate":0.01,
         "color":"蓝",
         "guarantee":[{
            "rarity":2,
            "count":3
         }]
      },{
         "level":4,
         "name":"金穗农场",
         "refreshRate":0.07,
         "chestRate":0.05,
         "color":"金",
         "guarantee":[{
            "rarity":3,
            "count":2
         }],
         "guaranteeCount":4
      },{
         "level":5,
         "name":"至臻农场",
         "refreshRate":0.015,
         "chestRate":0.3,
         "color":"红",
         "guarantee":[{
            "rarity":4,
            "count":2
         }],
         "guaranteeCount":14,
         "chestGuarantee":4
      }];
      
      public static const CROPS_COIN:Array = [{
         "id":1,
         "itemId":"8:1:1600",
         "name":"白萝卜",
         "rarity":1,
         "category":1,
         "value":1600
      },{
         "id":2,
         "itemId":"8:1:1600",
         "name":"大白菜",
         "rarity":1,
         "category":1,
         "value":1600
      },{
         "id":3,
         "itemId":"8:1:2000",
         "name":"黄瓜",
         "rarity":2,
         "category":1,
         "value":2000
      },{
         "id":4,
         "itemId":"8:1:2000",
         "name":"菠菜",
         "rarity":2,
         "category":1,
         "value":2000
      },{
         "id":5,
         "itemId":"8:1:2000",
         "name":"秋葵",
         "rarity":0,
         "category":1,
         "value":2000
      },{
         "id":6,
         "itemId":"8:1:2000",
         "name":"西兰花",
         "rarity":0,
         "category":1,
         "value":2000
      },{
         "id":7,
         "itemId":"8:1:2400",
         "name":"松茸",
         "rarity":3,
         "category":1,
         "value":2400
      },{
         "id":8,
         "itemId":"8:1:2400",
         "name":"黑松露",
         "rarity":3,
         "category":1,
         "value":2400
      },{
         "id":9,
         "itemId":"8:1:2800",
         "name":"黄金麦穗",
         "rarity":4,
         "category":1,
         "value":2800
      },{
         "id":10,
         "itemId":"8:1:2800",
         "name":"星辉玉米",
         "rarity":4,
         "category":1,
         "value":2800
      },{
         "id":11,
         "itemId":"8:1:3200",
         "name":"赤晶莓果",
         "rarity":5,
         "category":1,
         "value":3200
      },{
         "id":12,
         "itemId":"8:1:3200",
         "name":"红莲果",
         "rarity":5,
         "category":1,
         "value":3200
      }];
      
      public static const CROPS_SOUL:Array = [{
         "id":13,
         "itemId":"4:1384:3",
         "name":"白蘑菇",
         "rarity":1,
         "category":2,
         "value":3
      },{
         "id":14,
         "itemId":"4:1384:4",
         "name":"青椒",
         "rarity":2,
         "category":2,
         "value":4
      },{
         "id":15,
         "itemId":"4:1384:5",
         "name":"蓝莓",
         "rarity":0,
         "category":2,
         "value":5
      },{
         "id":16,
         "itemId":"4:1384:5",
         "name":"藏红花",
         "rarity":3,
         "category":2,
         "value":5
      },{
         "id":17,
         "itemId":"4:1384:6",
         "name":"辉光向日葵",
         "rarity":4,
         "category":2,
         "value":6
      },{
         "id":18,
         "itemId":"4:1384:7",
         "name":"凤血树",
         "rarity":5,
         "category":2,
         "value":7
      }];
      
      public static const FARM_FIELD_RARITY_RATES:Array = [[0,1,0,0,0,0],[0,0.5,0.5,0,0,0],[0,0.1,0.7,0.2,0,0],[0,0,0.4,0.4,0.2,0],[0,0,0,0.4,0.4,0.2]];
      
      public static const INVITE_BONUS:Array = [{
         "count":0,
         "cropMul":1,
         "dailyRefresh":4,
         "refreshMax":16,
         "dailyPick":2,
         "pickMax":8
      },{
         "count":1,
         "cropMul":1.1,
         "dailyRefresh":4,
         "refreshMax":16,
         "dailyPick":2,
         "pickMax":8
      },{
         "count":2,
         "cropMul":1.2,
         "dailyRefresh":4,
         "refreshMax":16,
         "dailyPick":2,
         "pickMax":8
      },{
         "count":3,
         "cropMul":1.3,
         "dailyRefresh":4,
         "refreshMax":20,
         "dailyPick":2,
         "pickMax":8
      },{
         "count":4,
         "cropMul":1.3,
         "dailyRefresh":5,
         "refreshMax":20,
         "dailyPick":2,
         "pickMax":8
      }];
      
      public static const CHEST_REWARDS:Array = [{
         "id":1,
         "itemId":"4:1336:3",
         "rate":0.2
      },{
         "id":2,
         "itemId":"8:1:60000",
         "rate":0.25
      },{
         "id":3,
         "itemId":"4:1384:100",
         "rate":0.15
      },{
         "id":4,
         "itemId":"4:3063:20",
         "rate":0.19
      },{
         "id":5,
         "itemId":"120:200069:1",
         "rate":0.01
      },{
         "id":6,
         "itemId":"19:43:1",
         "rate":0.2
      }];
      
      public static const FIELD_COUNT:int = 9;
      
      public static const FARM_COUNT:int = 5;
      
      public static const MAX_CHEST_REWARDS:int = 3;
      
      public static const MAX_SETTLE_CROPS:int = 32;
      
      public static const MIN_EFFECT_RARITY:int = 1;
      
      public static const SHARD_ID:String = "4:3255";
      
      public static const SHARD_PER_REFRESH:int = 4;
      
      public static const INVITE_EXPIRE_DAYS:int = 28;
      
      public static const MAX_INVITE_COUNT:int = 4;
      
      public static const WORKER_ANIM_INTERVAL:int = 60000;
      
      public static const WORKER_ANIM_SHOW_FRAMES:Array = [2,3];
      
      public static const INVITE_QS_KEY:String = "farminvite";
      
      public static const KEY:String = "farmv2";
      
      public static const CROP_ICON_ROTATION:Object = {
         10:-35,
         14:-25,
         2:-25,
         4:-35
      };
      
      public static const CROP_ICON_OFFSET_X:Object = {
         6:12,
         9:27
      };
      
      public static const CROP_ICON_OFFSET_Y:Object = {
         4:-7,
         5:10,
         8:14,
         15:12
      };
      
      public static const FIELD_CONVERT_RULES:Object = {
         "11:1":"8:1:140",
         "11:2":"4:3255:1",
         "11:3":"4:3255:2",
         "11:4":"4:3255:5",
         "11:5":"4:3255:5",
         "11:6":"4:3255:5",
         "11:7":"4:3255:2",
         "11:8":"4:3255:10",
         "11:9":"4:3255:1",
         "11:10":"4:3255:2",
         "11:11":"4:3255:5",
         "11:12":"4:3255:5",
         "11:13":"4:3255:5"
      };
      
      public static const SEED_CONVERT_RULES:Object = {
         "10:1":"8:1:20",
         "10:2":"8:1:10",
         "10:3":"8:1:10",
         "10:4":"8:1:20",
         "10:5":"8:1:25",
         "10:6":"8:1:150",
         "10:7":"8:1:25",
         "10:8":"8:1:15",
         "10:9":"8:1:15",
         "10:10":"8:1:10",
         "10:11":"8:1:25",
         "10:12":"8:1:18",
         "10:13":"8:1:20",
         "10:14":"8:1:15",
         "10:15":"8:1:170",
         "10:16":"8:1:200",
         "10:17":"8:1:244",
         "10:18":"8:1:100",
         "10:19":"8:1:250",
         "10:20":"8:1:100",
         "10:21":"8:1:25",
         "10:22":"8:1:30",
         "10:23":"8:1:50",
         "10:24":"8:1:120",
         "10:25":"8:1:181",
         "10:26":"8:1:240",
         "10:27":"8:1:280",
         "10:28":"8:1:400",
         "10:29":"8:1:100",
         "10:30":"8:1:333",
         "10:31":"8:1:30",
         "10:32":"8:1:36",
         "10:33":"8:1:61",
         "10:34":"8:1:131",
         "10:35":"8:1:208",
         "10:36":"8:1:272",
         "10:37":"8:1:309",
         "10:38":"8:1:440",
         "10:39":"8:1:360",
         "10:40":"8:1:400",
         "10:41":"4:102:25",
         "10:42":"4:103:25",
         "10:43":"4:104:50",
         "10:44":"4:105:10",
         "10:45":"4:106:60",
         "10:46":"4:114:20",
         "10:47":"8:1:120",
         "10:48":"4:118:20",
         "10:49":"4:122:20",
         "10:50":"4:123:20",
         "10:51":"4:124:20",
         "10:52":"4:125:20",
         "10:53":"4:126:20",
         "10:54":"4:131:10",
         "10:55":"4:151:20#4:6:2",
         "10:56":"8:1:30",
         "10:57":"8:1:36",
         "10:58":"8:1:30",
         "10:59":"8:1:39",
         "10:60":"8:1:68",
         "10:61":"8:1:74",
         "10:62":"4:225:15#4:7:1",
         "10:63":"4:226:34",
         "10:64":"8:1:15",
         "10:65":"8:1:15",
         "10:66":"8:1:15",
         "10:67":"8:1:15",
         "10:68":"8:1:15",
         "10:69":"4:300:24#138:90:100",
         "10:70":"4:301:16#138:90:500",
         "10:71":"8:1:50",
         "10:72":"8:1:75",
         "10:73":"4:3255:10",
         "10:74":"4:363:4",
         "10:75":"4:381:10#8:1:1000",
         "10:76":"4:382:10#138:90:200",
         "10:77":"8:1:14",
         "10:78":"8:1:15",
         "10:79":"4:435:15",
         "10:80":"4:444:26",
         "10:81":"8:1:25",
         "10:82":"8:1:40",
         "10:83":"4:643:30",
         "10:84":"4:727:30",
         "10:85":"4:741:20#19:13:2",
         "10:86":"4:801:25",
         "10:87":"8:1:188",
         "10:88":"4:1015:20",
         "10:89":"8:1:30",
         "10:90":"8:1:92",
         "10:91":"8:1:113",
         "10:92":"4:1264:10",
         "10:93":"4:1290:5",
         "10:94":"8:1:50",
         "10:95":"4:1355:1#8:1:500",
         "10:96":"4:3032:80",
         "10:97":"4:3140:20",
         "10:98":"4:3141:20",
         "10:99":"4:3142:20",
         "10:100":"4:3144:5",
         "10:101":"4:3145:20",
         "10:102":"4:3146:20",
         "10:103":"4:3147:20",
         "10:104":"4:3148:20",
         "10:105":"4:3178:5"
      };
      
      public static const BUY_PICK_PRICES:Array = [8,8,10,10,15,15];
      
      public static const BUY_REFRESH_PRICE:int = 4;
      
      public function FV2Config()
      {
         super();
      }
      
      public static function url(param1:String) : String
      {
         return PRE_URL + param1;
      }
      
      public static function cls(param1:String) : String
      {
         return param1;
      }
      
      public static function clsWithPrefix(param1:String) : String
      {
         return PRE_CLS + param1;
      }
      
      public static function isActStart() : Boolean
      {
         return DateUtil.getServerTimeInMS() > ActStartDate;
      }
      
      public static function getRarityColor(param1:int) : String
      {
         if(param1 == 0)
         {
            return "null";
         }
         if(param1 == 1)
         {
            return "white";
         }
         if(param1 == 2)
         {
            return "green";
         }
         if(param1 == 3)
         {
            return "purple";
         }
         if(param1 == 4)
         {
            return "gold";
         }
         return "red";
      }
      
      public static function getRarityName(param1:int) : String
      {
         var _loc2_:Array = ["不存在","白色","绿色","紫色","金色","红色"];
         if(param1 >= 0 && param1 < _loc2_.length)
         {
            return _loc2_[param1];
         }
         return "";
      }
      
      public static function getRarityUILevel(param1:int) : int
      {
         return param1;
      }
      
      public static function getFarmEffectUILevel(param1:int) : int
      {
         return [-1,1,1,2,3,3][param1];
      }
      
      public static function getPickBuyPrice(param1:int) : int
      {
         if(param1 >= BUY_PICK_PRICES.length)
         {
            return -1;
         }
         return BUY_PICK_PRICES[param1];
      }
      
      public static function getWeekTag() : String
      {
         var _loc1_:Date = new Date();
         var _loc2_:int = _loc1_.fullYear;
         var _loc3_:Date = new Date(_loc2_,0,1);
         var _loc4_:int = _loc3_.day == 0 ? 6 : int(_loc3_.day - 1);
         var _loc5_:int = Math.ceil((Math.floor((_loc1_.time - _loc3_.time) / 86400000) + _loc4_ + 1) / 7);
         return _loc2_ + (_loc5_ < 10 ? "0" : "") + _loc5_;
      }
      
      public static function getCropDefById(param1:int) : Object
      {
         var _loc3_:Object = null;
         var _loc2_:Array = CROPS_COIN.concat(CROPS_SOUL);
         for each(_loc3_ in _loc2_)
         {
            if(_loc3_["id"] == param1)
            {
               return _loc3_;
            }
         }
         throw new Error("找不到作物配置，cropId=" + param1);
      }
      
      public static function getInviteBonus(param1:int) : Object
      {
         if(param1 < 0)
         {
            throw new Error("邀请人数不能为负数，count=" + param1);
         }
         var _loc2_:int = Math.min(param1,INVITE_BONUS.length - 1);
         return INVITE_BONUS[_loc2_];
      }
      
      public static function getShardOutBySeedId(param1:String) : int
      {
         var _loc8_:String = null;
         var _loc9_:Array = null;
         var _loc10_:int = 0;
         var _loc11_:int = 0;
         var _loc12_:int = 0;
         var _loc2_:String = SEED_CONVERT_RULES[param1] as String;
         if(!_loc2_ || _loc2_.length == 0)
         {
            return 0;
         }
         var _loc3_:Array = SHARD_ID.split(":");
         var _loc4_:int = int(_loc3_[0]);
         var _loc5_:int = int(_loc3_[1]);
         var _loc6_:int = 0;
         var _loc7_:Array = _loc2_.split("#");
         for each(_loc8_ in _loc7_)
         {
            if(!(!_loc8_ || _loc8_.length == 0))
            {
               _loc9_ = _loc8_.split(":");
               if(_loc9_.length >= 3)
               {
                  _loc10_ = int(_loc9_[0]);
                  _loc11_ = int(_loc9_[1]);
                  _loc12_ = int(_loc9_[2]);
                  if(_loc10_ == _loc4_ && _loc11_ == _loc5_)
                  {
                     _loc6_ += _loc12_;
                  }
               }
            }
         }
         return _loc6_;
      }
      
      public static function calcFarmRewardRange(param1:int) : Object
      {
         if(param1 < 1 || param1 > FARM_DEFINE.length)
         {
            throw new Error("农场等级超出范围，farmLevel=" + param1 + "，有效范围：1-" + FARM_DEFINE.length);
         }
         var _loc2_:Array = FARM_FIELD_RARITY_RATES[param1 - 1];
         var _loc3_:int = FIELD_COUNT;
         var _loc4_:int = Math.floor(_loc3_ * 0.5);
         var _loc5_:int = Math.ceil(_loc3_ * 0.5);
         var _loc6_:Array = _avgValueByRarity(CROPS_COIN);
         var _loc7_:Array = _avgValueByRarity(CROPS_SOUL);
         var _loc8_:int = _minRarity(_loc2_);
         var _loc9_:int = _maxRarity(_loc2_);
         var _loc10_:int = Math.round(_loc4_ * _loc6_[_loc8_]);
         var _loc11_:int = Math.round(_loc5_ * _loc6_[_loc9_]);
         var _loc12_:int = Math.round(_loc4_ * _loc7_[_loc8_]);
         var _loc13_:int = Math.round(_loc5_ * _loc7_[_loc9_]);
         return {
            "coinMin":_loc10_,
            "coinMax":_loc11_,
            "soulMin":_loc12_,
            "soulMax":_loc13_,
            "chestRate":FARM_DEFINE[param1 - 1]["chestRate"]
         };
      }
      
      private static function _avgValueByRarity(param1:Array) : Array
      {
         var _loc4_:Object = null;
         var _loc5_:Array = null;
         var _loc6_:int = 0;
         var _loc7_:int = 0;
         var _loc2_:Array = [0,0,0,0,0,0];
         var _loc3_:Array = [0,0,0,0,0,0];
         for each(_loc4_ in param1)
         {
            _loc7_ = int(_loc4_["rarity"]);
            if(_loc7_ >= 0 && _loc7_ < 6)
            {
               _loc2_[_loc7_] += _loc4_["value"];
               ++_loc3_[_loc7_];
            }
         }
         _loc5_ = [0,0,0,0,0,0];
         _loc6_ = 0;
         while(_loc6_ < 6)
         {
            _loc5_[_loc6_] = _loc3_[_loc6_] > 0 ? Math.round(_loc2_[_loc6_] / _loc3_[_loc6_]) : 0;
            _loc6_++;
         }
         return _loc5_;
      }
      
      private static function _minRarity(param1:Array) : int
      {
         var _loc2_:int = 0;
         while(_loc2_ < param1.length)
         {
            if(param1[_loc2_] > 0)
            {
               return _loc2_;
            }
            _loc2_++;
         }
         return 0;
      }
      
      private static function _maxRarity(param1:Array) : int
      {
         var _loc2_:int = param1.length - 1;
         while(_loc2_ >= 0)
         {
            if(param1[_loc2_] > 0)
            {
               return _loc2_;
            }
            _loc2_--;
         }
         return 0;
      }
   }
}

