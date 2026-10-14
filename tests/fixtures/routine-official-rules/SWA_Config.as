package mmo.activityext.newact20220602.starwheeladventure.config
{
   import mmo.activityext.newact20220602.starwheeladventure.define.SWA_EssenceDefine;
   import mmo.activityext.newact20220602.starwheeladventure.define.SWA_GoodsDefine;
   import mmo.activityext.newact20220602.starwheeladventure.game.config.SWA_GameConfig;
   import mmo.common.DateUtil;
   
   public class SWA_Config
   {
      
      private static const PRE_URL:String = "newactivityext/newact20220602/starwheeladventure/starwheeladventure_";
      
      private static const PRE_CLS:String = "mmo.activityext.newact20220602.starwheeladventure.";
      
      public static const VipOneKeyTimesLimit:int = 1;
      
      public static const WEEK_PLAYED_TIMES_FOR_ONE_KEY:int = 2;
      
      public static const WEEK_PLAY_TARGET_TIMES:int = 3;
      
      public static const WEEK_PLAY_REWARD:String = "4:3194:40";
      
      public static const IS_PROGRESS_VALUE_ONLINE:Boolean = DateUtil.isAfterTimeWithDelayClose("20260227 02:00:00");
      
      public static const PROGRESS_VALUE_ARRAY:Array = [4,8,12];
      
      public static const PROGRESS_FRAME_ARRAY:Array = [28,63,100];
      
      public static const PROGRESS_PRIZE_ARRAY:Array = [{
         "noDesc":"聚星石*100",
         "bonus":"4:2076:100"
      },{
         "noDesc":"聚星精华*100",
         "bonus":"4:2078:100"
      },{
         "noDesc":"随机聚星精华*30",
         "bonus":"120:200109:30"
      }];
      
      public static const LIMIT_TYPE_KEY:Array = ["det","wet","tet"];
      
      public static const LIMIT_TYPE_DESC:Array = ["日限量","周限量","总限量"];
      
      public static const DAILY_GAME_TIMES:int = 3;
      
      public static const WEEKLY_GAME_TIMES:int = 6;
      
      public static const WEEKLY_EXCHANGE_GOODS_TIMES:int = 60;
      
      public static const COIN_INFO:String = SWA_GameConfig.PRIZE[1]["mcd"];
      
      private static const DoubleStartDate:String = "20220729 02:00:00";
      
      private static const DoubleEndDate:String = "20220805 02:00:00";
      
      public static const STORE_GOODS_ITEMS:Array = [new SWA_GoodsDefine(1,"时空精华","4:2077:2","4:2079:1","3:160",0,"",""),new SWA_GoodsDefine(2,"雷女精华","4:2077:2","4:2080:1","3:160",0,"",""),new SWA_GoodsDefine(3,"小诺精华","4:2077:2","4:2081:1","3:145",0,"",""),new SWA_GoodsDefine(4,"通灵师精华","4:2077:2","4:2082:1","2:50",0,"",""),new SWA_GoodsDefine(5,"次元精华","4:2077:2","4:2083:1","3:160",0,"",""),new SWA_GoodsDefine(6,"正理精华","4:2077:2","4:2084:1","3:160",0,"",""),new SWA_GoodsDefine(7,"希望精华","4:2077:2","4:2085:1","3:170",0,"",""),new SWA_GoodsDefine(8,"伊西多精华","4:2077:2","4:2086:1","3:145",0,"",""),new SWA_GoodsDefine(9,"以撒精华","4:2077:2","4:2087:1","3:160",0,"",""),new SWA_GoodsDefine(10,"超神龙精华","4:2077:2","4:2088:1","3:160",0,"",""),new SWA_GoodsDefine(11,"梵天精华","4:2077:2","4:2089:1","3:145",0,"",""),new SWA_GoodsDefine(12,"小炎精华","4:2077:2","4:2090:1","3:145",0,"",""),new SWA_GoodsDefine(13,"小天精华","4:2077:2","4:2091:1","3:145",0,"",""),new SWA_GoodsDefine(14,"安精华","4:2077:2","4:2092:1","3:160",0,"","")
      ,new SWA_GoodsDefine(15,"弥娅精华","4:2077:2","4:2093:1","3:160",0,"",""),new SWA_GoodsDefine(16,"王者龙精华","4:2077:2","4:2094:1","3:160",0,"",""),new SWA_GoodsDefine(17,"阿洛伊斯精华","4:2077:2","4:2095:1","3:160",0,"",""),new SWA_GoodsDefine(18,"异形族精华","4:2077:2","4:2096:1","3:145",0,"",""),new SWA_GoodsDefine(19,"阿修精华","4:2077:2","4:2098:1","3:145",0,"",""),new SWA_GoodsDefine(20,"记忆精华","4:2077:2","4:2099:1","3:160",0,"",""),new SWA_GoodsDefine(21,"诺雅精华","4:2077:2","4:2101:1","3:160",0,"",""),new SWA_GoodsDefine(22,"卡雅精华","4:2077:2","4:2102:1","3:145",0,"",""),new SWA_GoodsDefine(23,"盖西瑞精华","4:2077:2","4:2103:1","3:160",0,"",""),new SWA_GoodsDefine(24,"无烬龙精华","4:2077:2","4:2104:1","3:160",0,"",""),new SWA_GoodsDefine(25,"极精华","4:2077:2","4:2105:1","3:170",0,"",""),new SWA_GoodsDefine(26,"阿瑞精华","4:2077:2","4:2106:1","3:145",0,"",""),new SWA_GoodsDefine(27,"秩序精华","4:2077:2","4:2108:1","3:170",0,"",""),new SWA_GoodsDefine(28,"双生精华","4:2077:2","4:2110:1","3:170",0,"",""),new SWA_GoodsDefine(29,"黄金龙精华"
      ,"4:2077:2","4:2111:1","3:160",0,"",""),new SWA_GoodsDefine(30,"星绛精华","4:2077:2","4:2113:1","3:160",0,"",""),new SWA_GoodsDefine(31,"梦梦精华","4:2077:2","4:2112:1","2:50",0,"",""),new SWA_GoodsDefine(32,"召唤师精华","4:2077:2","4:2114:1","2:50",0,"",""),new SWA_GoodsDefine(33,"未来精华","4:2077:2","4:2116:1","3:160",0,"",""),new SWA_GoodsDefine(34,"莉莉丝精华","4:2077:2","4:2115:1","3:145",0,"",""),new SWA_GoodsDefine(35,"维蕾塔精华","4:2077:2","4:2117:1","3:160",0,"",""),new SWA_GoodsDefine(36,"路因加德精华","4:2077:2","4:2100:1","3:170",0,"",""),new SWA_GoodsDefine(37,"艾希精华","4:2077:2","4:2118:1","3:160",0,"",""),new SWA_GoodsDefine(38,"但丁精华","4:2077:2","4:2119:1","3:170",0,"",""),new SWA_GoodsDefine(39,"巴兰精华","4:2077:2","4:2120:1","3:160",0,"",""),new SWA_GoodsDefine(40,"蜜蕊可精华","4:2077:2","4:2121:1","3:170",0,"",""),new SWA_GoodsDefine(41,"梅丽精华","4:2077:2","4:2122:1","3:145",0,"","")];
      
      public static const PET_ESSENCE_ITEMS:Array = [new SWA_EssenceDefine(1,2076),new SWA_EssenceDefine(2,2077),new SWA_EssenceDefine(3,2078),new SWA_EssenceDefine(4,3194),new SWA_EssenceDefine(5,2079),new SWA_EssenceDefine(6,2080),new SWA_EssenceDefine(7,2081),new SWA_EssenceDefine(8,2082),new SWA_EssenceDefine(9,2083),new SWA_EssenceDefine(10,2084),new SWA_EssenceDefine(11,2085),new SWA_EssenceDefine(12,2086),new SWA_EssenceDefine(13,2087),new SWA_EssenceDefine(14,2088),new SWA_EssenceDefine(15,2089),new SWA_EssenceDefine(16,2090),new SWA_EssenceDefine(17,2091),new SWA_EssenceDefine(18,2092),new SWA_EssenceDefine(19,2093),new SWA_EssenceDefine(20,2094),new SWA_EssenceDefine(21,2095),new SWA_EssenceDefine(22,2096),new SWA_EssenceDefine(23,2097),new SWA_EssenceDefine(24,2098),new SWA_EssenceDefine(25,2099),new SWA_EssenceDefine(26,2100),new SWA_EssenceDefine(27,2101),new SWA_EssenceDefine(28,2102),new SWA_EssenceDefine(29,2103),new SWA_EssenceDefine(30,2104),new SWA_EssenceDefine(31,2105),new SWA_EssenceDefine(32
      ,2106),new SWA_EssenceDefine(33,2107),new SWA_EssenceDefine(34,2108),new SWA_EssenceDefine(35,2109),new SWA_EssenceDefine(36,2110),new SWA_EssenceDefine(37,2111),new SWA_EssenceDefine(38,2112),new SWA_EssenceDefine(39,2113),new SWA_EssenceDefine(40,2114),new SWA_EssenceDefine(41,2116),new SWA_EssenceDefine(42,2115),new SWA_EssenceDefine(43,2117),new SWA_EssenceDefine(44,2118),new SWA_EssenceDefine(45,2119),new SWA_EssenceDefine(46,2120),new SWA_EssenceDefine(47,2121),new SWA_EssenceDefine(48,2122),new SWA_EssenceDefine(49,2123),new SWA_EssenceDefine(50,2124),new SWA_EssenceDefine(51,2125),new SWA_EssenceDefine(52,2126),new SWA_EssenceDefine(53,2127),new SWA_EssenceDefine(54,2128),new SWA_EssenceDefine(55,2129),new SWA_EssenceDefine(56,2130),new SWA_EssenceDefine(57,2131),new SWA_EssenceDefine(58,2132),new SWA_EssenceDefine(59,2133),new SWA_EssenceDefine(60,2134),new SWA_EssenceDefine(61,2135),new SWA_EssenceDefine(62,2136),new SWA_EssenceDefine(63,2137),new SWA_EssenceDefine(64,2138),new SWA_EssenceDefine(65
      ,2139),new SWA_EssenceDefine(66,2140),new SWA_EssenceDefine(67,2141),new SWA_EssenceDefine(68,2142),new SWA_EssenceDefine(69,2143)];
      
      public function SWA_Config()
      {
         super();
      }
      
      public static function url(param1:String) : String
      {
         return PRE_URL + param1;
      }
      
      public static function clsWithPre(param1:String) : String
      {
         return PRE_CLS + param1;
      }
      
      public static function cls(param1:String) : String
      {
         return param1;
      }
      
      public static function get isEveryOneCanOneKeyFinish() : Boolean
      {
         return DateUtil.getServerTimeInMS() >= DateUtil.parseDate("20260213 02:00:00").getTime() && DateUtil.getServerTimeInMS() < DateUtil.parseDate("20260227 02:00:00").getTime();
      }
      
      public static function get IS_DOUBLE() : Boolean
      {
         return DateUtil.getServerTimeInMS() >= DateUtil.parseDate(SWA_Config.DoubleStartDate).getTime() && DateUtil.getServerTimeInMS() < DateUtil.parseDate(SWA_Config.DoubleEndDate).getTime();
      }
   }
}

