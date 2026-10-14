package mmo.arenav3.config
{
   import mmo.arenav3.defines.ArenaV3BattleFieldDefine;
   import mmo.arenav3.defines.ArenaV3ZoneDefine;
   import mmo.common.DateUtil;
   
   public class ArenaV3BattleFieldConfig
   {
      
      public static const FIELD_INDEX_COMMON:int = 0;
      
      public static const FIELD_INDEX_LUAN_DOU:int = 1;
      
      private static const OLD_YINGXIONG_FIELD_ZONE_RANK_DEFINES:Array = [{
         "groupId":1,
         "rankRange":[1,1],
         "title":"xx英雄"
      },{
         "groupId":2,
         "rankRange":[2,1000],
         "title":"黄金殿"
      },{
         "groupId":3,
         "rankRange":[1001,5000],
         "title":"白银殿"
      },{
         "groupId":4,
         "rankRange":[5001,50000],
         "title":"黑铁殿"
      },{
         "groupId":5,
         "rankRange":[50001,150000],
         "title":"青铜殿"
      },{
         "groupId":6,
         "rankRange":[150001,int.MAX_VALUE],
         "title":"白板殿"
      }];
      
      private static const OLD_CHAOSHEN_FIELD_ZONE_RANK_DEFINES:Array = [{
         "groupId":1,
         "rankRange":[1,1],
         "title":"xx战皇"
      },{
         "groupId":2,
         "rankRange":[2,50],
         "title":"王者殿堂"
      },{
         "groupId":3,
         "rankRange":[51,1000],
         "title":"钻石殿堂"
      },{
         "groupId":4,
         "rankRange":[1001,5000],
         "title":"红宝石殿堂"
      },{
         "groupId":5,
         "rankRange":[5001,50000],
         "title":"绿宝石殿堂"
      },{
         "groupId":6,
         "rankRange":[50001,150000],
         "title":"蓝宝石殿堂"
      },{
         "groupId":7,
         "rankRange":[150001,int.MAX_VALUE],
         "title":"水晶殿堂"
      }];
      
      public static const TITLE_TYPE:int = ArenaV3BattleFieldConfig_reachRankOncePrizes.TITLE_TYPE;
      
      public static const ENTER_JING_YING_PRIZE_TYPE:int = ArenaV3BattleFieldConfig_reachRankOncePrizes.ENTER_JING_YING_PRIZE_TYPE;
      
      public static const ENTER_JING_YING_PRIZE_DESC:String = "进入精英竞技场";
      
      private static const BATTLE_FIELD_CONFIG_ARRAY:Array = [{
         "id":1,
         "zoneDefines":[{
            "idx":0,
            "serverZoneId":1,
            "rankDefines":[{
               "groupId":1,
               "rankRange":[1,1],
               "title":"明日之星"
            },{
               "groupId":2,
               "rankRange":[2,1000],
               "title":"精兵堂"
            },{
               "groupId":3,
               "rankRange":[1001,10000],
               "title":"护卫堂"
            },{
               "groupId":4,
               "rankRange":[10001,100000],
               "title":"新兵堂"
            },{
               "groupId":5,
               "rankRange":[100001,int.MAX_VALUE],
               "title":"小兵堂"
            }],
            "desc":"旧版的精英竞技场"
         }],
         "desc":"旧版的精英战场"
      },{
         "id":2,
         "zoneDefines":[{
            "idx":0,
            "serverZoneId":11,
            "rankDefines":OLD_YINGXIONG_FIELD_ZONE_RANK_DEFINES,
            "desc":"流星战区"
         },{
            "idx":1,
            "serverZoneId":12,
            "rankDefines":OLD_YINGXIONG_FIELD_ZONE_RANK_DEFINES,
            "desc":"雷霆战区"
         },{
            "idx":2,
            "serverZoneId":13,
            "rankDefines":OLD_YINGXIONG_FIELD_ZONE_RANK_DEFINES,
            "desc":"冰雪战区"
         },{
            "idx":3,
            "serverZoneId":14,
            "rankDefines":OLD_YINGXIONG_FIELD_ZONE_RANK_DEFINES,
            "desc":"末日战区"
         }],
         "desc":"旧版英雄战场"
      },{
         "id":3,
         "zoneDefines":[{
            "idx":0,
            "serverZoneId":41,
            "rankDefines":OLD_CHAOSHEN_FIELD_ZONE_RANK_DEFINES,
            "desc":"时间战区"
         },{
            "idx":1,
            "serverZoneId":44,
            "rankDefines":OLD_CHAOSHEN_FIELD_ZONE_RANK_DEFINES,
            "desc":"生命战区"
         },{
            "idx":2,
            "serverZoneId":45,
            "rankDefines":OLD_CHAOSHEN_FIELD_ZONE_RANK_DEFINES,
            "desc":"力量战区"
         },{
            "idx":3,
            "serverZoneId":46,
            "rankDefines":OLD_CHAOSHEN_FIELD_ZONE_RANK_DEFINES,
            "desc":"是非战区"
         },{
            "idx":4,
            "serverZoneId":47,
            "rankDefines":OLD_CHAOSHEN_FIELD_ZONE_RANK_DEFINES,
            "desc":"智慧战区"
         }],
         "desc":"旧版超神战场-经典竞技场"
      },{
         "id":4,
         "zoneDefines":[{
            "idx":0,
            "serverZoneId":43,
            "rankDefines":OLD_CHAOSHEN_FIELD_ZONE_RANK_DEFINES,
            "desc":"红莲战区"
         },{
            "idx":1,
            "serverZoneId":48,
            "rankDefines":OLD_CHAOSHEN_FIELD_ZONE_RANK_DEFINES,
            "desc":"八荒战区"
         },{
            "idx":2,
            "serverZoneId":49,
            "rankDefines":OLD_CHAOSHEN_FIELD_ZONE_RANK_DEFINES,
            "desc":"寒魄战区"
         },{
            "idx":3,
            "serverZoneId":50,
            "rankDefines":OLD_CHAOSHEN_FIELD_ZONE_RANK_DEFINES,
            "desc":"龙魂战区"
         },{
            "idx":4,
            "serverZoneId":51,
            "rankDefines":OLD_CHAOSHEN_FIELD_ZONE_RANK_DEFINES,
            "desc":"极夜战区"
         }],
         "desc":"旧版超神战场-超杀竞技场"
      },{
         "id":5,
         "zoneDefines":[{
            "idx":0,
            "serverZoneId":52,
            "rankDefines":[{
               "groupId":1,
               "rankRange":[1,100],
               "title":"明日之星"
            },{
               "groupId":2,
               "rankRange":[101,1000],
               "title":"精兵堂"
            },{
               "groupId":3,
               "rankRange":[1001,5000],
               "title":"护卫堂"
            },{
               "groupId":4,
               "rankRange":[5001,10000],
               "title":"新兵堂"
            },{
               "groupId":5,
               "rankRange":[10001,int.MAX_VALUE],
               "title":"新手堂"
            }],
            "desc":"新星竞技场"
         }],
         "desc":"新星竞技场"
      },{
         "id":6,
         "zoneDefines":[{
            "idx":0,
            "serverZoneId":53,
            "rankDefines":JINGYING_FIELD_ZONE_RANK_DEFINES,
            "desc":"生命战区"
         },{
            "idx":1,
            "serverZoneId":54,
            "rankDefines":JINGYING_FIELD_ZONE_RANK_DEFINES,
            "desc":"是非战区"
         },{
            "idx":2,
            "serverZoneId":55,
            "rankDefines":JINGYING_FIELD_ZONE_RANK_DEFINES,
            "desc":"时间战区"
         },{
            "idx":3,
            "serverZoneId":56,
            "rankDefines":JINGYING_FIELD_ZONE_RANK_DEFINES,
            "desc":"力量战区"
         },{
            "idx":4,
            "serverZoneId":57,
            "rankDefines":JINGYING_FIELD_ZONE_RANK_DEFINES,
            "desc":"智慧战区"
         }],
         "desc":"经典竞技场"
      },{
         "id":7,
         "zoneDefines":[{
            "idx":0,
            "serverZoneId":58,
            "rankDefines":JINGYING_FIELD_ZONE_RANK_DEFINES_FOR_LUAN_DOU,
            "desc":"刚毅战区"
         },{
            "idx":1,
            "serverZoneId":59,
            "rankDefines":JINGYING_FIELD_ZONE_RANK_DEFINES_FOR_LUAN_DOU,
            "desc":"守护战区"
         },{
            "idx":2,
            "serverZoneId":60,
            "rankDefines":JINGYING_FIELD_ZONE_RANK_DEFINES_FOR_LUAN_DOU,
            "desc":"超能战区"
         },{
            "idx":3,
            "serverZoneId":61,
            "rankDefines":JINGYING_FIELD_ZONE_RANK_DEFINES_FOR_LUAN_DOU,
            "desc":"热血战区"
         },{
            "idx":4,
            "serverZoneId":62,
            "rankDefines":JINGYING_FIELD_ZONE_RANK_DEFINES_FOR_LUAN_DOU,
            "desc":"真知战区"
         }],
         "desc":"乱斗竞技场"
      },{
         "id":8,
         "zoneDefines":[{
            "idx":0,
            "serverZoneId":100,
            "rankDefines":BEST_RANK_DEFINES,
            "desc":"希望战区"
         },{
            "idx":1,
            "serverZoneId":101,
            "rankDefines":BEST_RANK_DEFINES,
            "desc":"终湮战区"
         },{
            "idx":2,
            "serverZoneId":102,
            "rankDefines":BEST_RANK_DEFINES,
            "desc":"归墟战区"
         }],
         "reachRankOncePrizes":ArenaV3BattleFieldConfig_reachRankOncePrizes.ONCE_PRIZES_VERSION,
         "dailyFreeChallengeTime":DAILY_FREE_FIGHT_TIME_NORMAL,
         "dailyMaxBuyChallengeTime":20,
         "buyChallengeTimePrice":10,
         "dailyRefreshTime":5,
         "enterMinBattlePower":150000,
         "clearCdPrice":5,
         "seasonId":ArenaV3Config.curVersionId,
         "seasonOpenTime":ArenaV3Config.seasonOpenTime,
         "dailyCloseTime":"24:00:00",
         "weeklySettleCloseTime":"21:00:00",
         "desc":"经典竞技场",
         "rule":"•经典玩法，只可上阵星迹、神运和灵初时代精灵\n" + "（进攻方挑战他人结果为平局时视为进攻方失败）\n" + "•挑战比自己高排名对手获胜可更换排名，挑战比自己低排名的对手胜利或失败不会交换排名\n" + "•布置守阵和进攻时，可自由选择使用自己养成或系统养成\n" + "•每日2个竞技场各有8次挑战机会，每日前8次挑战（经典和传奇竞技场共用），无论胜负均可以获得15竞技币\n" + "•竞技场<font color=\'#E46537\'>周五</font>至次<font color=\'#E46537\'>周三</font>每天<font color=\'#E46537\'>0点关闭</font>，<font color=\'#E46537\'>每周四</font>结算日提前至<font color=\'#E46537\'>21点</font>关闭"
      },{
         "id":9,
         "zoneDefines":[{
            "idx":0,
            "serverZoneId":200,
            "rankDefines":BEST_RANK_DEFINES,
            "desc":"未来战区"
         },{
            "idx":1,
            "serverZoneId":201,
            "rankDefines":BEST_RANK_DEFINES,
            "desc":"造化战区"
         },{
            "idx":2,
            "serverZoneId":202,
            "rankDefines":BEST_RANK_DEFINES,
            "desc":"誓约战区"
         }],
         "reachRankOncePrizes":ArenaV3BattleFieldConfig_reachRankOncePrizes.ONCE_PRIZES_VERSION,
         "dailyFreeChallengeTime":DAILY_FREE_FIGHT_TIME_NORMAL,
         "dailyMaxBuyChallengeTime":20,
         "buyChallengeTimePrice":10,
         "dailyRefreshTime":5,
         "enterMinBattlePower":150000,
         "clearCdPrice":5,
         "seasonId":ArenaV3Config.curVersionId,
         "seasonOpenTime":ArenaV3Config.seasonOpenTime,
         "dailyCloseTime":"24:00:00",
         "weeklySettleCloseTime":"21:30:00",
         "desc":"传奇竞技场",
         "rule":"<font color=\'#CC6600\' size=\'16\'>#RULE#</font>\n" + "•传奇玩法：只可上阵星迹及以上时代的精灵\n" + "•挑战比自己高排名对手获胜可更换排名，挑战比自己低排名的对手胜利或失败不会交换排名\n" + "•每日2个竞技场各有8次挑战机会，每日前8次挑战（经典和传奇竞技场共用），无论胜负均可以获得15竞技币\n" + "•竞技场<font color=\'#E46537\'>周五</font>至次<font color=\'#E46537\'>周三</font>每天<font color=\'#E46537\'>0点关闭</font>，<font color=\'#E46537\'>每周四</font>结算日提前至<font color=\'#E46537\'>21点</font>关闭"
      }];
      
      public static const BATTLE_FIELD_DEFINES:Vector.<ArenaV3BattleFieldDefine> = ArenaV3BattleFieldDefine.createWithObjArr(BATTLE_FIELD_CONFIG_ARRAY);
      
      public static const OLD_VERSION_CHAO_SHEN_FIELDS:Vector.<ArenaV3BattleFieldDefine> = new Vector.<ArenaV3BattleFieldDefine>();
      
      public static const XIN_XING_BATTLE_FIELD:ArenaV3BattleFieldDefine = BATTLE_FIELD_DEFINES[4];
      
      public static const JING_DIAN_BATTLE_FIELD:ArenaV3BattleFieldDefine = BATTLE_FIELD_DEFINES[7];
      
      public static const LUAN_DOU__BATTLE_FIELD:ArenaV3BattleFieldDefine = BATTLE_FIELD_DEFINES[8];
      
      public static const NEW_BATTLE_FIELD_DEFINES:Vector.<ArenaV3BattleFieldDefine> = new Vector.<ArenaV3BattleFieldDefine>();
      
      public static const JING_YING_BATTLE_FIELDS:Vector.<ArenaV3BattleFieldDefine> = new Vector.<ArenaV3BattleFieldDefine>();
      
      JING_YING_BATTLE_FIELDS[0] = JING_DIAN_BATTLE_FIELD;
      JING_YING_BATTLE_FIELDS[1] = LUAN_DOU__BATTLE_FIELD;
      NEW_BATTLE_FIELD_DEFINES[0] = JING_DIAN_BATTLE_FIELD;
      NEW_BATTLE_FIELD_DEFINES[1] = LUAN_DOU__BATTLE_FIELD;
      OLD_VERSION_CHAO_SHEN_FIELDS[0] = BATTLE_FIELD_DEFINES[2];
      OLD_VERSION_CHAO_SHEN_FIELDS[1] = BATTLE_FIELD_DEFINES[3];
      
      public function ArenaV3BattleFieldConfig()
      {
         super();
      }
      
      private static function get isWeekendDay() : Boolean
      {
         var _loc1_:Date = DateUtil.getServerTime();
         return _loc1_.day == 5 || _loc1_.day == 6 || _loc1_.day == 0;
      }
      
      private static function get DAILY_FREE_FIGHT_TIME_NORMAL() : int
      {
         return 8;
      }
      
      private static function get DAILY_FREE_FIGHT_TIME_SPECIAL() : int
      {
         return 8;
      }
      
      private static function get JINGYING_FIELD_ZONE_RANK_DEFINES() : Array
      {
         var _loc1_:Array = [{
            "groupId":1,
            "rankRange":[1,5],
            "title":"xx战皇",
            "mcds":"4:1344:140#4:2076:100#138:90:10000"
         },{
            "groupId":2,
            "rankRange":[6,100],
            "title":"荣耀殿堂",
            "mcds":"4:1344:120#4:2076:100#138:90:8000"
         },{
            "groupId":3,
            "rankRange":[101,1000],
            "title":"王者殿堂",
            "mcds":"4:1344:100#4:2076:80#138:90:6000"
         },{
            "groupId":4,
            "rankRange":[1001,5000],
            "title":"大师殿堂",
            "mcds":"4:1344:80#4:2076:60#138:90:5000"
         },{
            "groupId":5,
            "rankRange":[5001,50000],
            "title":"钻石殿堂",
            "mcds":"4:1344:60#4:2076:40#138:90:4000"
         },{
            "groupId":6,
            "rankRange":[50001,150000],
            "title":"黄金殿堂",
            "mcds":"4:1344:40#4:2076:30#138:90:1000"
         },{
            "groupId":7,
            "rankRange":[150001,int.MAX_VALUE],
            "title":"青铜殿堂",
            "mcds":"4:1344:20#4:2076:30#138:90:500"
         }];
         var _loc2_:Array = [{
            "groupId":1,
            "rankRange":[1,5],
            "title":"xx战皇",
            "mcds":"4:1344:140#4:2076:100#138:90:10000"
         },{
            "groupId":2,
            "rankRange":[6,100],
            "title":"荣耀殿堂",
            "mcds":"4:1344:120#4:2076:100#138:90:8000"
         },{
            "groupId":3,
            "rankRange":[101,1000],
            "title":"王者殿堂",
            "mcds":"4:1344:100#4:2076:80#138:90:6000"
         },{
            "groupId":4,
            "rankRange":[1001,5000],
            "title":"大师殿堂",
            "mcds":"4:1344:80#4:2076:60#138:90:5000"
         },{
            "groupId":5,
            "rankRange":[5001,50000],
            "title":"钻石殿堂",
            "mcds":"4:1344:60#4:2076:40#138:90:4000"
         },{
            "groupId":6,
            "rankRange":[50001,150000],
            "title":"黄金殿堂",
            "mcds":"4:1344:40#4:2076:30#138:90:1000"
         },{
            "groupId":7,
            "rankRange":[150001,int.MAX_VALUE],
            "title":"青铜殿堂",
            "mcds":"4:1344:20#4:2076:30#138:90:500"
         }];
         if(ArenaV3Config.isWeekPrizeChange)
         {
            return _loc2_;
         }
         return _loc1_;
      }
      
      private static function get JINGYING_FIELD_ZONE_RANK_DEFINES_FOR_LUAN_DOU() : Array
      {
         var _loc1_:Array = [{
            "groupId":1,
            "rankRange":[1,5],
            "title":"xx战皇",
            "mcds":"4:1344:140#4:1384:100#138:90:10000"
         },{
            "groupId":2,
            "rankRange":[6,100],
            "title":"荣耀殿堂",
            "mcds":"4:1344:120#4:1384:100#138:90:8000"
         },{
            "groupId":3,
            "rankRange":[101,1000],
            "title":"王者殿堂",
            "mcds":"4:1344:100#4:1384:80#138:90:6000"
         },{
            "groupId":4,
            "rankRange":[1001,5000],
            "title":"大师殿堂",
            "mcds":"4:1344:80#4:1384:60#138:90:5000"
         },{
            "groupId":5,
            "rankRange":[5001,50000],
            "title":"钻石殿堂",
            "mcds":"4:1344:60#4:1384:40#138:90:4000"
         },{
            "groupId":6,
            "rankRange":[50001,150000],
            "title":"黄金殿堂",
            "mcds":"4:1344:40#4:1384:30#138:90:1000"
         },{
            "groupId":7,
            "rankRange":[150001,int.MAX_VALUE],
            "title":"青铜殿堂",
            "mcds":"4:1344:20#4:1384:30#138:90:500"
         }];
         var _loc2_:Array = [{
            "groupId":1,
            "rankRange":[1,5],
            "title":"xx战皇",
            "mcds":"4:1344:140#4:1384:100#138:90:10000"
         },{
            "groupId":2,
            "rankRange":[6,100],
            "title":"荣耀殿堂",
            "mcds":"4:1344:120#4:1384:100#138:90:8000"
         },{
            "groupId":3,
            "rankRange":[101,1000],
            "title":"王者殿堂",
            "mcds":"4:1344:100#4:1384:80#138:90:6000"
         },{
            "groupId":4,
            "rankRange":[1001,5000],
            "title":"大师殿堂",
            "mcds":"4:1344:80#4:1384:60#138:90:5000"
         },{
            "groupId":5,
            "rankRange":[5001,50000],
            "title":"钻石殿堂",
            "mcds":"4:1344:60#4:1384:40#138:90:4000"
         },{
            "groupId":6,
            "rankRange":[50001,150000],
            "title":"黄金殿堂",
            "mcds":"4:1344:40#4:1384:30#138:90:1000"
         },{
            "groupId":7,
            "rankRange":[150001,int.MAX_VALUE],
            "title":"青铜殿堂",
            "mcds":"4:1344:20#4:1384:30#138:90:500"
         }];
         if(ArenaV3Config.isWeekPrizeChange)
         {
            return _loc2_;
         }
         return _loc1_;
      }
      
      private static function get BEST_RANK_DEFINES() : Array
      {
         var _loc1_:Array = [{
            "groupId":1,
            "rankRange":[1,100],
            "mcds":"4:1344:320#4:3250:3"
         },{
            "groupId":2,
            "rankRange":[101,1000],
            "mcds":"4:1344:300#4:3250:3"
         },{
            "groupId":3,
            "rankRange":[1001,2000],
            "mcds":"4:1344:280#4:3250:3"
         },{
            "groupId":4,
            "rankRange":[2001,4000],
            "mcds":"4:1344:250#4:3250:2"
         },{
            "groupId":5,
            "rankRange":[4001,8000],
            "mcds":"4:1344:220#4:3250:2"
         },{
            "groupId":6,
            "rankRange":[8001,15000],
            "mcds":"4:1344:180#4:3250:1"
         },{
            "groupId":7,
            "rankRange":[15001,50000],
            "mcds":"4:1344:140#4:3250:1"
         },{
            "groupId":8,
            "rankRange":[50001,int.MAX_VALUE],
            "mcds":"4:1344:100"
         }];
         var _loc2_:Array = [{
            "groupId":1,
            "rankRange":[1,100],
            "mcds":"4:1344:320#4:3250:3"
         },{
            "groupId":2,
            "rankRange":[101,1000],
            "mcds":"4:1344:300#4:3250:3"
         },{
            "groupId":3,
            "rankRange":[1001,2000],
            "mcds":"4:1344:280#4:3250:3"
         },{
            "groupId":4,
            "rankRange":[2001,4000],
            "mcds":"4:1344:250#4:3250:2"
         },{
            "groupId":5,
            "rankRange":[4001,8000],
            "mcds":"4:1344:220#4:3250:2"
         },{
            "groupId":6,
            "rankRange":[8001,15000],
            "mcds":"4:1344:180#4:3250:1"
         },{
            "groupId":7,
            "rankRange":[15001,50000],
            "mcds":"4:1344:140#4:3250:1"
         },{
            "groupId":8,
            "rankRange":[50001,int.MAX_VALUE],
            "mcds":"4:1344:100"
         }];
         if(ArenaV3Config.isWeekPrizeChange)
         {
            return _loc2_;
         }
         return _loc1_;
      }
      
      public static function getFieldDefineVecByTypeIdx(param1:int) : ArenaV3BattleFieldDefine
      {
         return JING_YING_BATTLE_FIELDS[param1];
      }
      
      public static function getFieldDefineById(param1:int) : ArenaV3BattleFieldDefine
      {
         var fieldId:int = param1;
         var tmpDefines:Vector.<ArenaV3BattleFieldDefine> = BATTLE_FIELD_DEFINES.filter(function(param1:ArenaV3BattleFieldDefine, param2:int, param3:*):*
         {
            return param1.id == fieldId;
         });
         return tmpDefines.length == 0 ? null : tmpDefines[0];
      }
      
      public static function getFieldDefineByServerZoneId(param1:int) : ArenaV3BattleFieldDefine
      {
         var serverZoneId:int = param1;
         var tmpDefines:Vector.<ArenaV3BattleFieldDefine> = BATTLE_FIELD_DEFINES.filter(function(param1:ArenaV3BattleFieldDefine, param2:int, param3:*):*
         {
            var _loc4_:* = param1.getZoneDefineByServerId(serverZoneId);
            return _loc4_ != null;
         });
         return tmpDefines.length == 0 ? null : tmpDefines[0];
      }
      
      public static function getArenaNameByServerZoneId(param1:int) : String
      {
         var serverZoneId:int = param1;
         var arenaName:String = "";
         BATTLE_FIELD_DEFINES.forEach(function(param1:ArenaV3BattleFieldDefine, param2:int, param3:*):*
         {
            var _loc4_:ArenaV3ZoneDefine = param1.getZoneDefineByServerId(serverZoneId);
            if(_loc4_ != null)
            {
               arenaName = _loc4_.desc;
            }
         });
         return arenaName;
      }
      
      public static function getIsInJingyingField(param1:int) : Boolean
      {
         var jingyingFieldIds:Array = null;
         var curField:int = param1;
         jingyingFieldIds = [];
         JING_YING_BATTLE_FIELDS.forEach(function(param1:ArenaV3BattleFieldDefine, param2:int, param3:*):*
         {
            jingyingFieldIds[param2] = param1.id;
         });
         if(jingyingFieldIds.indexOf(curField) != -1)
         {
            return true;
         }
         return false;
      }
      
      public static function get SEASON_WILL_CHANGED_FIELD_DEFINE() : ArenaV3BattleFieldDefine
      {
         return JING_DIAN_BATTLE_FIELD;
      }
   }
}

